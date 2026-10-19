// Receive a new kernel over the network and start it.
//
// push.py on the Mac sends kernel.img to UDP port 5001 in numbered pieces,
// one at a time, each acknowledged before the next goes ("stop and wait":
// simple, and on a 0.5 ms link still about 2 MB/s). Every packet starts
// with the same 16 bytes, little-endian:
//
//   magic "KPSH"   kind (1 start, 2 data, 3 go)   offset or size   CRC or length
//
// The pieces land at 0x200000, where the SPL puts a kernel from the card
// (free now: we copied ourselves to 0x02000000 at start-up). "Go" checks
// the CRC and the ARM64 header, and then we jump there: the new kernel
// copies itself over us, exactly as after a cold boot.
//
// To keep it, the Mac also sends the 2 KB "LOADER" header that goes in
// front of the kernel on the card (kind 4, two halves). Then, before
// jumping, we check the header describes exactly what we received, write
// both to the card where the SPL looks (sector 16384), and read it back.
// Only if all of that worked do we start the new kernel.

#include <stdint.h>
#include "console.h"
#include "exception.h"
#include "gic.h"
#include "loader.h"
#include "log.h"
#include "../spl/sd.h"
#include "usb.h"

#define MAGIC   0x4853504b   // "KPSH"
#define START   1
#define DATA    2
#define GO      3
#define HEADER  4
#ifndef LOAD_AT
#define LOAD_AT 0x200000UL
#endif
#define MAX_SIZE (16U << 20)

// Replies: magic, kind, offset, status
#define OK        0
#define TOO_BIG   1
#define BAD_PIECE 2
#define BAD_CRC   3
#define NOT_ARM64 4
#define NO_START  5
#define BAD_HEADER 6
#define CARD_FAILED 7
#define NOT_OURS  8

#define LOADER_SECTOR 16384   // as in the SPL

static struct {
	uint32_t size, crc, received, pieces, pushes, last_status;
	int expecting;
	uint32_t header_got;   // bytes of header received (2048 = all)
	const char *card;      // what happened at the last save
} st;

static uint8_t header[2048] __attribute__((aligned(16)));
static uint32_t sector[128];

void chainload(uintptr_t entry);   // chain.S

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

// CRC-32, the one zip and Python's zlib.crc32 use: a bit at a time (no
// table), about 10 ms for our 80 KB
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t crc = ~0U;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}

// Rockchip's CRC-32, the one in the LOADER header (as in the SPL)
static uint32_t crc32_rk(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0;
	while (n--) {
		crc ^= (uint32_t)*p++ << 24;
		for (int i = 0; i < 8; i++)
			crc = crc & 0x80000000 ? crc << 1 ^ 0x04c10db7 : crc << 1;
	}
	return crc;
}

static int is_loader(const uint8_t *p)
{
	static const char magic[8] = "LOADER  ";
	for (int i = 0; i < 8; i++)
		if (p[i] != magic[i])
			return 0;
	return 1;
}

// Header and kernel onto the card. 0, or what went wrong.
static uint32_t save(const uint8_t *kernel)
{
	if (!is_loader(header) || get32(header + 0x10) != LOAD_AT ||
	    get32(header + 0x14) != st.size || get32(header + 0x18) != crc32_rk(kernel, st.size))
		return st.card = "header doesn't match", BAD_HEADER;
	if (sd_init())
		return st.card = "card didn't start", CARD_FAILED;
	// Only over a kernel already there: never over anything else
	if (sd_read(LOADER_SECTOR, 1, sector))
		return st.card = "card read failed", CARD_FAILED;
	if (!is_loader((const uint8_t *)sector))
		return st.card = "no kernel there", NOT_OURS;
	uint32_t sectors = (st.size + 511) / 512;
	if (sd_write(LOADER_SECTOR, 4, header) || sd_write(LOADER_SECTOR + 4, sectors, kernel))
		return st.card = "card write failed", CARD_FAILED;
	// Read it all back
	for (uint32_t s = 0; s < 4 + sectors; s++) {
		if (sd_read(LOADER_SECTOR + s, 1, sector))
			return st.card = "read-back failed", CARD_FAILED;
		const uint8_t *want = s < 4 ? header + s * 512 : kernel + (s - 4) * 512;
		uint32_t n = s < 4 ? 512 : st.size - (s - 4) * 512;
		if (n > 512)
			n = 512;
		const uint8_t *got = (const uint8_t *)sector;
		for (uint32_t i = 0; i < n; i++)
			if (got[i] != want[i])
				return st.card = "read back differs", CARD_FAILED;
	}
	st.card = "saved";
	return OK;
}

uint32_t loader_packet(const uint8_t *data, uint32_t len, uint8_t *reply, int *go)
{
	*go = 0;
	if (len < 16 || get32(data) != MAGIC)
		return 0;
	uint32_t kind = get32(data + 4), a = get32(data + 8), b = get32(data + 12);
	uint32_t status = OK;
	uint8_t *dst = (uint8_t *)LOAD_AT;

	if (kind == START) {
		if (a == 0 || a > MAX_SIZE) {
			status = TOO_BIG;
		} else {
			st.size = a, st.crc = b, st.received = 0, st.pieces = 0;
			st.expecting = 1, st.header_got = 0;
			klog("loader: receiving a kernel, %u bytes", a);
		}
	} else if (kind == DATA) {
		if (!st.expecting)
			status = NO_START;
		else if (b != len - 16 || a > st.size || b > st.size - a)
			status = BAD_PIECE;
		else {
			for (uint32_t i = 0; i < b; i++)
				dst[a + i] = data[16 + i];
			// Count each piece once, even if the Mac sends it again (our
			// acknowledgement got lost)
			if (a == st.received) {
				st.received += b;
				st.pieces++;
			}
		}
	} else if (kind == HEADER) {
		if (!st.expecting)
			status = NO_START;
		else if (b != len - 16 || a > sizeof header || b > sizeof header - a)
			status = BAD_PIECE;
		else {
			for (uint32_t i = 0; i < b; i++)
				header[a + i] = data[16 + i];
			if (a == st.header_got)
				st.header_got += b;
		}
	} else if (kind == GO) {
		if (!st.expecting || st.received != st.size)
			status = NO_START;
		else if (crc32(dst, st.size) != st.crc)
			status = BAD_CRC;
		else if (st.size < 64 || get32(dst + 56) != 0x644d5241)   // "ARM\x64"
			status = NOT_ARM64;
		else if (st.header_got == sizeof header && (status = save(dst)) != OK)
			klog("loader: not saved to the card: %s", st.card);
		else {
			st.pushes++;
			if (st.header_got == sizeof header)
				klog("loader: CRC ok, saved to the card, starting it");
			else
				klog("loader: CRC ok, starting the new kernel (not saved)");
			*go = 1;
		}
		if (status)
			klog("loader: refused (%u)", status);
	} else {
		return 0;
	}
	st.last_status = status;
	put32(reply, MAGIC);
	put32(reply + 4, kind);
	put32(reply + 8, a);
	put32(reply + 12, status);
	return 16;
}

// Leave: the Mac must see us go (so it resets the link for the new kernel),
// the interrupt we're in the middle of must be finished in the GIC (the new
// kernel's GIC set-up wouldn't clear it), and the timer stopped
void loader_start_new(void)
{
	usb_stop();
	if (irq_current != GIC_SPURIOUS)
		gic_done(irq_current);
	__asm__ volatile("msr cntp_ctl_el0, xzr\nisb");
	chainload(LOAD_AT);
}

void loader_lines(uint32_t row)
{
	console_goto(0, row);
	console_color(GREY);
	kputs("Loader   ");
	console_color(WHITE);
	if (st.expecting)
		kprintf("%u of %u bytes        \n", st.received, st.size);
	else
		kputs("waiting for make push\n");
}
