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

#include <stdint.h>
#include "console.h"
#include "exception.h"
#include "gic.h"
#include "loader.h"
#include "log.h"
#include "usb.h"

#define MAGIC   0x4853504b   // "KPSH"
#define START   1
#define DATA    2
#define GO      3
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

static struct {
	uint32_t size, crc, received, pieces, pushes, last_status;
	int expecting;
} st;

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
			st.expecting = 1;
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
	} else if (kind == GO) {
		if (!st.expecting || st.received != st.size)
			status = NO_START;
		else if (crc32(dst, st.size) != st.crc)
			status = BAD_CRC;
		else if (st.size < 64 || get32(dst + 56) != 0x644d5241)   // "ARM\x64"
			status = NOT_ARM64;
		else {
			st.pushes++;
			klog("loader: CRC ok, starting the new kernel");
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
