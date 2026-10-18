// Bring the RAM to life: 1 GB of DDR3, two 16-bit chips side by side on a
// 32-bit bus, at 333 MHz.
//
// Three blocks inside the chip do the work, and all three need setting up:
//
//   CPU, GPU, display...  ->  scheduler (MSCH)  ->  controller (uMCTL2)  ->  PHY  ->  RAM chips
//
//   - the controller (Synopsys uMCTL2) turns "read this address" into DDR
//     commands: open a row, read a column, close the row, and every 7.8 us
//     refresh everything (DRAM forgets otherwise). It must leave exactly the
//     right number of clock cycles between commands: the "timings".
//   - the PHY (Innosilicon) is the analog side: it drives the 50-odd wires
//     at the right strength and catches the data coming back. The data
//     arrives a little late, by an amount that depends on the board's
//     wiring, so the PHY measures it: "gate training".
//   - the scheduler (MSCH) picks whose request goes next and needs a copy
//     of the timings too.
//
// The order follows U-Boot's PX30 driver (drivers/ram/rockchip/sdram_px30.c),
// itself an open version of Rockchip's blob, and every value matches what
// Rockchip's blob left in the registers (kernel/dumps/). Unlike both, we
// don't detect the RAM's size: we know this board.

#include <stdint.h>
#include "dram.h"
#include "report.h"

#define PCTL    0xff600000UL   // the controller
#define PHY     0xff2a0000UL
#define MSCH    0xff530000UL   // the scheduler
#define DDR_GRF 0xff630000UL   // odds and ends for the RAM blocks
#define CRU     0xff2b0000UL   // clocks and resets
#define PMUGRF  0xff010000UL
#define GRF     0xff140000UL

#define PHY_REG(n) (PHY + 4 * (n))

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void set_bits(uintptr_t a, uint32_t b) { wr(a, rd(a) | b); }
static void clr_bits(uintptr_t a, uint32_t b) { wr(a, rd(a) & ~b); }
static void clr_set(uintptr_t a, uint32_t c, uint32_t s) { wr(a, (rd(a) & ~c) | s); }

uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

void udelay(uint32_t us)
{
	uint64_t t = ticks();
	while (ticks() - t < us * 24ULL)
		;
}

static struct ddr_report *const rep = (struct ddr_report *)DDR_REPORT;
struct ddr_report early;   // filled in SRAM; copied to RAM at the end

// ---- clocks and resets ------------------------------------------------------

// The four resets, in CRU_SOFTRST_CON1/2 (1 = held in reset). "p" is each
// block's register interface, the other its logic.
static void resets(int ctl, int ctl_p, int phy, int phy_p)
{
	wr(CRU + 0x304, (0x70U << 16) | ctl << 4 | ctl << 5 | ctl_p << 6);
	wr(CRU + 0x308, (0x05U << 16) | phy | phy_p << 2);
}

// The DPLL: 24 MHz x 83 / 3 = 664 MHz. The PHY halves it: the RAM's clock
// is 332 MHz, and DDR moves data on both edges, so 664 million transfers
// a second on each of the 32 data wires: 2.6 GB/s.
static int dpll_start(void)
{
	wr(CRU + 0xa0, 3U << (4 + 16) | 0 << 4);   // DPLL_MODE: crystal, while it changes
	wr(CRU + 0x20, 7U << (12 + 16) | 3 << 12 | 0xfffU << 16 | 83);   // postdiv1 3, fbdiv 83
	wr(CRU + 0x24, 1U << (12 + 16) | 1 << 12 |     // integer mode, no fractions
	   7U << (6 + 16) | 1 << 6 | 0x3fU << 16 | 1); // postdiv2 1, refdiv 1
	uint64_t t = ticks();
	while (!(rd(CRU + 0x24) & 1 << 10))            // locked?
		if (ticks() - t > 24000 * 10)              // it took 628 us on this board
			return -1;
	early.dpll_lock_us = (ticks() - t) / 24;
	wr(CRU + 0xa0, 3U << (4 + 16) | 1 << 4);   // DPLL_MODE: the PLL
	return 0;
}

// ---- the controller ---------------------------------------------------------

// Written while the controller is still in reset. Offsets and values: U-Boot's
// sdram-px30-ddr3-detect-333.inc, with MSTR adjusted for our 32-bit bus.
static const uint32_t controller[][2] = {
	{ 0x000, 0x83040001 },   // MSTR: DDR3, burst of 8, x16 chips, full 32-bit bus
	{ 0x064, 0x0028003b },   // RFSHTMG: refresh every 7.8 us, each takes 350 ns
	{ 0x0d0, 0x00020053 },   // INIT0-5: the power-up waits (500 us of reset, ...)
	{ 0x0d4, 0x00020000 },
	{ 0x0d8, 0x00000100 },
	{ 0x0dc, 0x03200000 },   // INIT3: mode registers MR0 0x320 (CAS latency 6,
	{ 0x0e0, 0x00000000 },   //   write recovery 5, burst 8) and MR1 0; INIT4: MR2, MR3
	{ 0x0e4, 0x00090000 },
	{ 0x0f4, 0x000f012f },   // RANKCTL
	{ 0x100, 0x07090b06 },   // DRAMTMG0-8: the gaps between commands, in clock
	{ 0x104, 0x00050209 },   //   cycles: row open to read, read to precharge,
	{ 0x108, 0x03030407 },   //   write to read...
	{ 0x10c, 0x00202006 },
	{ 0x110, 0x03020204 },
	{ 0x114, 0x03030202 },
	{ 0x120, 0x00000903 },
	{ 0x180, 0x00800020 },   // ZQCTL0: recalibrate the drivers now and then
	{ 0x184, 0x00000000 },
	{ 0x190, 0x07010001 },   // DFITMG0: timing of the controller-PHY interface ("DFI")
	{ 0x198, 0x07000101 },
	{ 0x1a0, 0xc0400003 },
	{ 0x240, 0x06000604 },   // ODTCFG/ODTMAP: when the chips switch on their termination
	{ 0x244, 0x00000201 },
	{ 0x250, 0x00001f00 },   // SCHED
	{ 0x490, 0x00000001 },   // PCTRL_0: open the controller's port
};

// The address map: which bits of an address pick the column, bank and row.
// 1 GB = 2^30 bytes: 2 bits pick a byte in a 32-bit word, 10 a column,
// 3 a bank and 15 a row (2 + 10 + 3 + 15 = 30). Lowest to highest: column,
// bank, row ("ddrconfig 2"), so walking through memory stays in one open
// row as long as possible. 0x0f/0x1f/0x3f mean "unused".
static const uint32_t address_map[] = {
	0x0000001f,   // ADDRMAP0: no second rank
	0x00080808,   // ADDRMAP1: bank bits
	0x00000000,   // ADDRMAP2-3: column bits
	0x00000000,
	0x00001f1f,   // ADDRMAP4
	0x07070707,   // ADDRMAP5-7: row bits; rows 15-17 don't exist
	0x0f070707,
	0x00000f0f,
	0x00003f3f,   // ADDRMAP8
};

static void controller_setup(void)
{
	for (uint32_t i = 0; i < sizeof controller / sizeof controller[0]; i++)
		wr(PCTL + controller[i][0], controller[i][1]);
	// Power saving: power down after 13 idle cycles (x32), self-refresh
	// after 93 (x32)
	clr_set(PCTL + 0x34, 0xffU << 16 | 0x1f, 93U << 16 | 13);
	clr_set(PCTL + 0x38, 0xfffU << 16, 5U << 16);
	set_bits(PCTL + 0x180, 1U << 31);   // no automatic recalibration yet
	for (uint32_t i = 0; i < 9; i++)
		wr(PCTL + 0x200 + 4 * i, address_map[i]);
}

// ---- the PHY ----------------------------------------------------------------

// Small timing corrections ("skew") per wire, in the PHY's delay steps:
// the address/command wires, then per byte lane its 8 data wires, the mask
// and the strobe. U-Boot's sdram-px30-ddr_skew.inc; the blob used the same.
static const uint8_t skew_ca[15] = {
	0x77, 0x88, 0x79, 0x79, 0x87, 0x97, 0x87, 0x78,
	0x77, 0x78, 0x87, 0x88, 0x87, 0x87, 0x77,
};
static const uint8_t skew_even[11] = {   // byte lanes 0 and 2
	0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x69, 0x09,
};
static const uint8_t skew_lane1[11] = {
	0x77, 0x78, 0x77, 0x78, 0x77, 0x78, 0x77, 0x78, 0x77, 0x79, 0x09,
};
static const uint8_t skew_lane3[11] = {
	0x77, 0x78, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x79, 0x09,
};

static void phy_reset(void)
{
	clr_bits(PHY_REG(0), 3 << 2);   // analog and digital parts in reset
	udelay(1);
	set_bits(PHY_REG(0), 1 << 2);   // analog out
	udelay(5);
	set_bits(PHY_REG(0), 1 << 3);   // digital out
	udelay(1);
}

static void phy_setup(void)
{
	// The DLLs that delay the strobes: at 400 MHz and under, bypassed
	set_bits(PHY_REG(0x13), 1 << 4);
	clr_bits(PHY_REG(0x14), 1 << 3);
	for (int i = 0; i < 4; i++) {
		set_bits(PHY_REG(0x26 + i * 0x10), 1 << 4);
		clr_bits(PHY_REG(0x27 + i * 0x10), 1 << 3);
	}
	set_bits(PHY_REG(0xa4), 0x1f);
	for (int i = 0; i < 4; i++)
		wr(PHY_REG(0x28 + i * 0x10), 2);

	wr(PHY_REG(0x01), 0x0a);   // DDR3
	wr(PHY_REG(0x0a), 6);      // CAS latency 6 (as in MR0)
	wr(PHY_REG(0x0b), 0);      // additive latency 0
	wr(PHY_REG(0x0c), 5);      // CAS write latency 5 (as in MR2)
	clr_set(PHY_REG(0), 0xf << 4, 0xf << 4);   // all 4 byte lanes: 32 bits

	// Drive strengths: commands 34 ohm, clock 45 ohm, data 34 ohm, and
	// 225 ohm termination when receiving (the device tree's 0x15, 0x12, 0x02)
	wr(PHY_REG(0x11), 21);
	clr_set(PHY_REG(0x12), 0x1f << 3, 21 << 3);
	wr(PHY_REG(0x16), 18);
	wr(PHY_REG(0x18), 18);
	for (int i = 0; i < 4; i++) {
		uint32_t j = 0x20 + i * 0x10;
		wr(PHY_REG(j), 21);
		wr(PHY_REG(j + 0xf), 21);
		wr(PHY_REG(j + 0x1), 2);
		wr(PHY_REG(j + 0xe), 2);
	}

	set_bits(PHY_REG(2), 8);   // deskew on
	for (int i = 0; i < 15; i++)
		wr(PHY_REG(0xb0 + i), skew_ca[i]);
	// The same per-lane values for rank 0 (0x70) and the unused rank 1 (0xc0)
	for (uint32_t base = 0x70; base <= 0xc0; base += 0x50)
		for (int lane = 0; lane < 4; lane++) {
			const uint8_t *s = lane == 1 ? skew_lane1 : lane == 3 ? skew_lane3 : skew_even;
			for (int i = 0; i < 11; i++)
				wr(PHY_REG(base + lane * 11 + i), s[i]);
		}
}

// Gate training: the PHY sends reads and finds, per byte lane, when the
// strobe really comes back, so it opens its "read gate" at the right
// moment. Returns 0 if every lane found it.
static int phy_gate_training(void)
{
	uint32_t odt = rd(PHY_REG(0x2e));
	for (int i = 0; i < 4; i++) {   // termination fixed at 225 ohm meanwhile
		wr(PHY_REG(0x21 + i * 0x10), 2);
		wr(PHY_REG(0x2e + i * 0x10), 0);
	}
	clr_set(PHY_REG(2), 0x33, 0x20);       // rank 0
	clr_set(PHY_REG(2), 0x33, 0x20 | 1);   // go
	udelay(50);
	uint32_t result = rd(PHY_REG(0xff));
	clr_set(PHY_REG(2), 0x33, 0x20);
	clr_bits(PHY_REG(2), 0x30);
	for (int i = 0; i < 4; i++) {
		wr(PHY_REG(0x21 + i * 0x10), odt);
		wr(PHY_REG(0x2e + i * 0x10), odt);
	}
	// bit 4: error; bits 0-3: a 1 for each lane that finished, which
	// should match the lanes in use (PHY register 0, bits 4-7)
	if (result & 0x10)
		return -1;
	return (result & 0xf) ^ (rd(PHY_REG(0)) >> 4 & 0xf) ? -1 : 0;
}

// Training with the controller's own background work paused (refresh,
// recalibration and power saving would get in the way)
static int training(void)
{
	uint32_t pwrctl = rd(PCTL + 0x30);
	wr(PCTL + 0x30, 0);
	set_bits(PCTL + 0x60, 1);          // RFSHCTL3: auto-refresh off...
	wr(PCTL + 0x60, rd(PCTL + 0x60) ^ 2);   // ...and tell the controller
	int err = phy_gate_training();
	clr_bits(PCTL + 0x60, 1);
	wr(PCTL + 0x60, rd(PCTL + 0x60) ^ 2);
	wr(PCTL + 0x30, pwrctl);
	return err;
}

// ---- the scheduler and the summary ------------------------------------------

static void scheduler_setup(void)
{
	wr(MSCH + 0x08, 2 | 2 << 8);          // DEVICECONF: ddrconfig 2, as the address map
	wr(GRF + 0x534, 3U << (14 + 16));     // SOC_NOC_CON1: bits 14-15 to 0
	wr(MSCH + 0x0c, 1024 / 64);           // DEVICESIZE: 1 GB, in 64 MB units
	wr(MSCH + 0x10, 0x290b0609);          // the same timings, in its own format
	wr(MSCH + 0x14, 0x08020401);
	wr(MSCH + 0x18, 0x00000002);
	wr(MSCH + 0x1c, 0x00001111);
	wr(MSCH + 0x110, 0x0000000c);
	wr(MSCH + 0x114, 0x00000222);
	wr(MSCH + 0x1000, 0xff);              // "aging": old requests get priority
	for (int i = 0; i < 4; i++)
		wr(MSCH + 0x1040 + 4 * i, 0xff);

	// The summary for later stages, in the format U-Boot and Linux decode
	// (sdram_common.h): os_reg2 = DDR3, 1 rank, 1024 columns, 8 banks,
	// 32768 rows, 32-bit bus of x16 chips; os_reg3 = format version 2.
	wr(PMUGRF + 0x208, 0x000062b1);
	wr(PMUGRF + 0x20c, 0x20000001);
}

static void low_power_setup(void)
{
	wr(DDR_GRF + 0x04, 0x1f1f0617);   // let the controller gate its own clocks when idle
	wr(DDR_GRF + 0x20, 7U << 16 | 1 | 1U << (9 + 16) | 1 << 9);
	set_bits(PHY_REG(7), 1 << 7);     // PHY: stop its digital clock in power-down
	set_bits(PCTL + 0x30, 1 << 1 | 1 << 0 | 1 << 3);   // power-down, self-refresh on
}

// ---- all of it --------------------------------------------------------------

int dram_init(void)
{
	resets(1, 1, 1, 1);    // everything in reset
	udelay(10);
	resets(1, 1, 1, 0);    // the PHY's registers out (its PLL needs them)
	if (dpll_start())
		return DRAM_ERR_PLL;
	resets(1, 1, 0, 0);    // the PHY out: it now clocks the controller
	udelay(10);
	phy_reset();
	resets(1, 0, 0, 0);    // the controller's registers out
	controller_setup();
	phy_setup();
	set_bits(PCTL + 0x1b0, 1 << 5 | 1 << 4);   // DFIMISC: start the PHY when we let go

	// Let go: the controller starts the PHY, waits for it, then wakes the
	// RAM chips up (reset, clock, mode registers, calibration) on its own
	uint64_t t = ticks();
	resets(0, 0, 0, 0);
	while (!(rd(PCTL + 0x04) & 7))   // STAT: operating mode no longer "init"
		if (ticks() - t > 24000 * 100)
			return DRAM_ERR_INIT;
	early.init_us = (ticks() - t) / 24;

	// Train; U-Boot retries if the gates land outside 0x20-0x80
	for (;;) {
		early.training_tries++;
		int err = training();
		int ok = !err;
		for (int i = 0; i < 4; i++) {
			early.gates[i] = rd(PHY_REG(0xfb + i));
			if (early.gates[i] < 0x20 || early.gates[i] > 0x80)
				ok = 0;
		}
		if (ok)
			break;
		if (early.training_tries >= 10)
			return DRAM_ERR_TRAINING;
	}

	scheduler_setup();
	low_power_setup();
	return 0;
}

// ---- the test ---------------------------------------------------------------

// Two checks. First the address wires: write a different value at each
// power-of-two address and read them all back. A missing or crossed address
// bit makes two of them land in the same place. Then the data: a pseudo-
// random pattern over 1 MB, written then read, to catch stuck or crossed
// data bits. Returns 0, or the failing address.
uintptr_t dram_test(void)
{
	// Address 0 through a variable the compiler can't see into: C treats a
	// plain 0 pointer as "never valid", but here it's the first word of RAM
	uintptr_t zero;
	__asm__("mov %0, #0" : "=r"(zero));
	volatile uint32_t *const m = (volatile uint32_t *)zero;
	m[0] = 0x0a0a0a0a;
	for (uint32_t bit = 2; bit < 30; bit++)
		m[(1UL << bit) / 4] = 0xa5000000 | bit;
	if (m[0] != 0x0a0a0a0a)
		return 4;   // 0 is fine; report the first write that hit it as 4
	for (uint32_t bit = 2; bit < 30; bit++)
		if (m[(1UL << bit) / 4] != (0xa5000000 | bit))
			return 1UL << bit;
	early.test_words = 29;

	volatile uint32_t *p = (volatile uint32_t *)0x08000000;   // 128 MB in
	uint32_t x = 0x12345678;
	for (uint32_t i = 0; i < 0x40000; i++) {
		x ^= x << 13, x ^= x >> 17, x ^= x << 5;   // xorshift
		p[i] = x;
	}
	x = 0x12345678;
	for (uint32_t i = 0; i < 0x40000; i++) {
		x ^= x << 13, x ^= x >> 17, x ^= x << 5;
		if (p[i] != x)
			return (uintptr_t)&p[i];
	}
	early.test_words += 0x40000;
	return 0;
}

void dram_report(void)
{
	early.magic = DDR_MAGIC;
	early.ticks_out = ticks();
	*rep = early;
}
