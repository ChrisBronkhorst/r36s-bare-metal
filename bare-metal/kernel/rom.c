// The boot ROM: the one piece of code on this console that isn't ours.
//
// It's burned into the chip at 0xffff0000, and the CPU starts there at
// power-on: it finds a boot medium, loads our RAM setup into SRAM, then our
// SPL. We can't change it, but we can read it. Maybe: chips often hide
// their ROM from the non-secure world (us, at EL2) after boot. So each word
// is read directly first, and if that's refused, our EL3 monitor reads it
// for us from the secure world.
//
// Y saves it to the card (sector 12416 on), for the Mac to disassemble.

#include <stdint.h>
#include "console.h"
#include "exception.h"
#include "monitor.h"
#include "rom.h"
#include "../spl/sd.h"

#define BOOT_ROM      0xffff0000UL
#define ROM_SIZE      0x10000UL     // 64 KB: the whole window, to be sure
#define ROM_SECTOR    12416         // after the RAM dump (12288, 64 sectors)
#define ROM_SECTORS   (1 + ROM_SIZE / 512)   // a header, then the ROM
#define ROM_MAGIC     0x31504d554d4f52UL     // "ROMDUMP1"

// Sector 0: the header. Then the 64 KB, word by word, 0 where unreadable.
struct rom_header {
	uint64_t magic;
	uint32_t base, size;
	uint32_t direct, via_el3, unreadable;   // words read each way
	uint32_t first_refused;                 // offset of the first refusal, or ~0
};

static uint32_t buf[ROM_SECTORS * 128] __attribute__((aligned(64)));
static int done;

static uint32_t careful_read(uintptr_t a)
{
	register uint32_t v __asm__("w0") = 0;
	__asm__ volatile("ldr %w0, [%1]" : "+r"(v) : "r"(a) : "memory");
	return v;
}

static void read_rom(void)
{
	struct rom_header *h = (struct rom_header *)buf;
	uint32_t *rom = buf + 128;
	for (uint32_t i = 0; i < 128; i++)
		buf[i] = 0;
	h->magic = ROM_MAGIC;
	h->base = BOOT_ROM;
	h->size = ROM_SIZE;
	h->first_refused = ~0U;
	int ours = monitor_ours();
	survive_faults = 1;
	for (uint32_t off = 0; off < ROM_SIZE; off += 4) {
		uint64_t faults = fault_count;
		uint32_t v = careful_read(BOOT_ROM + off);
		if (fault_count == faults) {
			rom[off / 4] = v;
			h->direct++;
			continue;
		}
		if (h->first_refused == ~0U)
			h->first_refused = off;
		int ok = 0;
		v = ours ? monitor_rom_read(off, &ok) : 0;
		rom[off / 4] = ok ? v : 0;
		if (ok)
			h->via_el3++;
		else
			h->unreadable++;
	}
	survive_faults = 0;
	done = 1;
}

static void label(const char *name)
{
	console_color(GREY);
	uint32_t n = 0;
	for (; name[n]; n++)
		kputc(name[n]);
	for (; n < 9; n++)
		kputc(' ');
	console_color(WHITE);
}

// Where the readable part ends: the last 1 KB block that isn't all one value
static uint32_t rom_end(const uint32_t *rom)
{
	uint32_t end = 0;
	for (uint32_t b = 0; b < ROM_SIZE / 1024; b++) {
		const uint32_t *w = rom + b * 256;
		for (int i = 1; i < 256; i++)
			if (w[i] != w[0]) {
				end = (b + 1) * 1024;
				break;
			}
	}
	return end;
}

void rom_page(void)
{
	if (!done)
		read_rom();
	const struct rom_header *h = (const struct rom_header *)buf;
	const uint32_t *rom = buf + 128;
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("The boot ROM, at ffff0000\n");
	label("Read");
	kprintf("%u words directly,\n", h->direct);
	label("");
	kprintf("%u via EL3, %u not at all\n", h->via_el3, h->unreadable);
	if (h->first_refused != ~0U) {
		label("Refused");
		kprintf("from +%x on\n", h->first_refused);
	}
	label("Content");
	kprintf("about %u KB\n", rom_end(rom) / 1024);
	label("First");
	for (int i = 0; i < 4; i++)
		kprintf("%08x ", rom[i]);
	kputc('\n');

	// The longest run of printable text: version strings, often
	uint32_t best = 0, best_len = 0, start = 0, len = 0;
	const uint8_t *b = (const uint8_t *)rom;
	for (uint32_t i = 0; i < ROM_SIZE; i++) {
		if (b[i] >= 0x20 && b[i] < 0x7f) {
			if (!len++)
				start = i;
			if (len > best_len)
				best = start, best_len = len;
		} else {
			len = 0;
		}
	}
	label("Text");
	if (best_len >= 4) {
		kprintf("+%x: \"", best);
		for (uint32_t i = 0; i < best_len && i < 28; i++)
			kputc((char)b[best + i]);
		kputs(best_len > 28 ? "...\"\n" : "\"\n");
	} else {
		kputs("none\n");
	}
	console_goto(0, 22);
	console_color(YELLOW);
	kputs("Y: save to card  A: buttons");
}

// 0 when saved; otherwise what went wrong
const char *rom_save(void)
{
	if (!done)
		read_rom();
	if (sd_init())
		return "card didn't start";
	// Only over our own earlier dump, or blank card: never over anything else
	static uint32_t old[128];
	if (sd_read(ROM_SECTOR, 1, old))
		return "card read failed";
	int blank = 1;
	for (int i = 0; i < 128; i++)
		if (old[i])
			blank = 0;
	if (!blank && (old[0] | (uint64_t)old[1] << 32) != ROM_MAGIC)
		return "that card area isn't empty";
	if (sd_write(ROM_SECTOR, ROM_SECTORS, buf))
		return "card write failed";
	for (uint32_t s = 0; s < ROM_SECTORS; s++) {
		if (sd_read(ROM_SECTOR + s, 1, old))
			return "read-back failed";
		for (int i = 0; i < 128; i++)
			if (old[i] != buf[s * 128 + i])
				return "read back differs";
	}
	return 0;
}
