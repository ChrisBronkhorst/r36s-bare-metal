// What Rockchip's RAM setup (the "DDR blob", stage 1 of the boot) did.
//
// The RAM is driven by three blocks inside the chip:
//   - the memory controller (Synopsys "uMCTL2", at 0xff600000): turns reads
//     and writes into DDR commands (open a row, read a column, refresh...)
//     with the right timings between them
//   - the PHY (Innosilicon, at 0xff2a0000): the analog side, which drives
//     the wires at the right strength and samples the data at the right
//     moment, learnt by "training"
//   - the memory scheduler (MSCH, at 0xff530000): decides whose request
//     (CPU, GPU, display...) goes next, and where addresses land
// plus a PLL for the clock (DPLL). All of it is still set up the way the
// blob left it, so we can read its choices back. The blob also leaves a
// summary for later stages in two "OS registers" (PMUGRF os_reg2/3), in a
// format U-Boot and Linux decode too.
//
// Y saves every one of those registers to a spare area of the card
// (sectors 12288 on), for the Mac to read: an answer key for our own RAM
// setup.

#include <stdint.h>
#include "console.h"
#include "dump.h"
#include "exception.h"
#include "ram.h"
#include "../spl/sd.h"

#define PCTL    0xff600000UL
#define PHY     0xff2a0000UL
#define MSCH    0xff530000UL
#define DDR_GRF 0xff630000UL
#define CRU     0xff2b0000UL
#define PMUCRU  0xff2bc000UL
#define PMUGRF  0xff010000UL
#define GRF     0xff140000UL

#define DUMP_SECTOR  12288   // 6 MB in: after the boot ROM's area, before uboot.img
#define DUMP_SECTORS 64      // 32 KB
#define DUMP_MAGIC   0x31504d554d4152UL   // "RAMDUMP1"

static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

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

static const char *type_name(uint32_t t)
{
	switch (t) {
	case 0: return "DDR4";
	case 2: return "DDR2";
	case 3: return "DDR3";
	case 5: return "LPDDR2";
	case 6: return "LPDDR3";
	case 7: return "LPDDR4";
	}
	return "unknown";
}

void ram_page(void)
{
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("RAM, as Rockchip's setup left it\n");

	// The summary, decoded like U-Boot's sdram_common.h (channel 0)
	uint32_t r2 = rd(PMUGRF + 0x208), r3 = rd(PMUGRF + 0x20c);
	uint32_t type = r2 >> 13 & 7;
	uint32_t rank = 1 + (r2 >> 11 & 1);
	uint32_t col = 9 + (r2 >> 9 & 3);
	uint32_t bk = 3 - (r2 >> 8 & 1);              // log2 of the bank count
	uint32_t bw = 2 >> (r2 >> 2 & 3);             // log2 of bytes per transfer
	uint32_t dbw = 2 >> (r2 & 3);                 // the same, per chip
	uint32_t row0 = ((((r2 >> 6 & 3) | (r3 >> 5 & 1) << 2) + 1) & 7) + 12;
	uint32_t row1 = ((((r2 >> 4 & 3) | (r3 >> 4 & 1) << 2) + 1) & 7) + 12;
	label("Summary");
	kprintf("%08x %08x\n", r2, r3);
	label("Type");
	kprintf("%s (%u)\n", type_name(type), type);
	label("Shape");
	kprintf("%u rank%s, %u-bit bus, x%u chips\n", rank, rank > 1 ? "s" : "", 8 << bw, 8 << dbw);
	label("");
	kprintf("%u banks, %u cols, %u rows\n", 1 << bk, 1 << col, 1 << row0);
	uint64_t cs0 = 1ULL << (row0 + col + bk + bw);
	uint64_t cs1 = rank > 1 ? 1ULL << (row1 + col + bk + bw) : 0;
	if (r3 >> 30 & 1)   // row_3_4: only 3/4 of the top row bit is there
		cs0 = cs0 * 3 / 4, cs1 = cs1 * 3 / 4;
	label("Size");
	kprintf("%lu MB", (cs0 + cs1) >> 20);
	if (rank > 1)
		kprintf(" (%lu + %lu)", cs0 >> 20, cs1 >> 20);
	kputc('\n');

	// The clock: the DPLL runs at twice the RAM clock (the PHY divides it)
	uint32_t dpll = dpll_khz();
	label("Clock");
	kprintf("DPLL %u MHz: RAM at %u MHz,\n", dpll / 1000, dpll / 2000);
	label("");
	kprintf("%u million transfers/s\n", dpll / 1000);

	// The controller: what kind of RAM it thinks it's driving
	uint32_t mstr = rd(PCTL + 0x0);
	label("MSTR");
	kprintf("%08x: %s%s%s%s, ranks %x\n", mstr,
		mstr & 1 ? "DDR3 " : "", mstr & 8 ? "LPDDR3 " : "",
		mstr & 0x10 ? "DDR4 " : "", mstr & 4 ? "LPDDR2 " : "", mstr >> 24 & 3);
	label("MR0/1");
	kprintf("%08x  MR2/3 %08x\n", rd(PCTL + 0xdc), rd(PCTL + 0xe0));
	label("Refresh");
	kprintf("RFSHTMG %08x\n", rd(PCTL + 0x64));
	label("Timings");
	kprintf("%08x %08x %08x\n", rd(PCTL + 0x100), rd(PCTL + 0x104), rd(PCTL + 0x108));
	label("Map");
	kprintf("conf %08x size %08x\n", rd(MSCH + 0x8), rd(MSCH + 0xc));

	// The PHY's training results: per byte lane, when to open the read gate
	label("Gates");
	for (int i = 0; i < 4; i++)
		kprintf("%02x ", rd(PHY + 4 * (0xfb + i)) & 0xff);
	kputs("(good: 20-80)\n");
	label("Drive");
	kprintf("cmd %02x clk %02x dqs %02x odt %02x\n", rd(PHY + 4 * 0x11) & 0xff,
		rd(PHY + 4 * 0x16) & 0xff, rd(PHY + 4 * 0x20) & 0xff, rd(PHY + 4 * 0x21) & 0xff);

	console_goto(0, 22);
	console_color(YELLOW);
	kputs("Y: save registers to card  A: buttons");
}

// ---- the dump --------------------------------------------------------------

struct region { char name[8]; uint32_t base, size; };
static const struct region regions[] = {
	{ "PCTL",    PCTL,             0x500 },
	{ "PHY",     PHY,              0x400 },
	{ "MSCH",    MSCH,             0x120 },
	{ "MSCHAGE", MSCH + 0x1000,    0x50 },
	{ "DDR_GRF", DDR_GRF,          0x100 },
	{ "CRU",     CRU,              0x400 },
	{ "PMUCRU",  PMUCRU,           0x100 },
	{ "PMUGRF",  PMUGRF,           0x240 },
	{ "GRF",     GRF,              0x1000 },
};
#define REGIONS (sizeof regions / sizeof regions[0])

// The layout on the card: a 512-byte header, then each region's words
struct dump_header {
	uint64_t magic;
	uint32_t count, faults;
	struct { char name[8]; uint32_t base, size, offset, pad; } r[REGIONS];
};

static uint32_t buf[DUMP_SECTORS * 128] __attribute__((aligned(64)));

// Read one register, surviving a refusal (the word then reads deadbeef)
static uint32_t careful_read(uintptr_t a)
{
	register uint32_t v __asm__("w0") = 0xdeadbeef;
	__asm__ volatile("ldr %w0, [%1]" : "+r"(v) : "r"(a) : "memory");
	return v;
}

// 0 when saved; otherwise what went wrong
const char *ram_save(void)
{
	struct dump_header *h = (struct dump_header *)buf;
	for (uint32_t i = 0; i < sizeof buf / 4; i++)
		buf[i] = 0;
	h->magic = DUMP_MAGIC;
	h->count = REGIONS;
	uint32_t at = 512;
	uint64_t faults = fault_count;
	survive_faults = 1;
	for (uint32_t i = 0; i < REGIONS; i++) {
		for (int j = 0; j < 8; j++)
			h->r[i].name[j] = regions[i].name[j];
		h->r[i].base = regions[i].base;
		h->r[i].size = regions[i].size;
		h->r[i].offset = at;
		for (uint32_t o = 0; o < regions[i].size; o += 4)
			buf[(at + o) / 4] = careful_read(regions[i].base + o);
		at += regions[i].size;
	}
	survive_faults = 0;
	h->faults = fault_count - faults;

	if (sd_init())
		return "card didn't start";
	// Only over our own earlier dump, or blank card: never over anything else
	static uint32_t old[128];
	if (sd_read(DUMP_SECTOR, 1, old))
		return "card read failed";
	int blank = 1;
	for (int i = 0; i < 128; i++)
		if (old[i])
			blank = 0;
	if (!blank && (old[0] | (uint64_t)old[1] << 32) != DUMP_MAGIC)
		return "that card area isn't empty";
	if (sd_write(DUMP_SECTOR, DUMP_SECTORS, buf))
		return "card write failed";
	// Read it back, to be sure
	for (uint32_t s = 0; s < DUMP_SECTORS; s++) {
		if (sd_read(DUMP_SECTOR + s, 1, old))
			return "read-back failed";
		for (int i = 0; i < 128; i++)
			if (old[i] != buf[s * 128 + i])
				return "read back differs";
	}
	return 0;
}
