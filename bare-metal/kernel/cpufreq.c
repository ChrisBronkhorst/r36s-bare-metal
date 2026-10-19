// The CPU's clock: the "APLL", one of the chip's clock generators.
//
// A PLL multiplies the 24 MHz crystal up and divides it down again:
//   out = 24 MHz / refdiv * fbdiv / postdiv1 / postdiv2
// The middle part (24 / refdiv * fbdiv, the "VCO") must stay between about
// 950 and 3800 MHz, so slow speeds come from dividing a fast VCO.
//
// A faster clock needs a higher voltage: transistors switch faster with
// more push behind them. Too little and the CPU gets the occasional wrong
// answer, which is much worse than a crash. So going up we raise the
// voltage first, and going down we lower it last. The pairs below are the
// ones Linux uses for this chip (px30.dtsi); the PLL settings are from its
// clock driver (clk-px30.c).
//
// While the PLL is being reprogrammed it can't be used: we switch it to
// "slow mode" (the CPU runs straight from the 24 MHz crystal), change it,
// wait until it reports "locked" (stable at the new speed), and switch
// back. Everything else keeps working; it takes well under a millisecond.

#include <stdint.h>
#include "cpufreq.h"
#include "i2c.h"
#include "rk817.h"
#include "sysreg.h"

#define CRU          0xff2b0000UL
#define APLL_CON(i)  (CRU + 4 * (i))
#define CRU_MODE     (CRU + 0xa0)
#define CLKSEL0      (CRU + 0x100)
#define PLL_LOCK     (1U << 10)   // in APLL_CON1

static const struct speed {
	uint32_t mhz, mv, refdiv, fbdiv, postdiv1, postdiv2;
} speeds[] = {
	{  408,  950, 1, 68, 2, 2 },   // VCO 1632
	{  600,  950, 1, 75, 3, 1 },   // VCO 1800
	{  816, 1050, 1, 68, 2, 1 },   // VCO 1632
	{ 1008, 1175, 1, 84, 2, 1 },   // VCO 2016
	{ 1200, 1300, 1, 50, 1, 1 },   // VCO 1200
	{ 1296, 1350, 1, 54, 1, 1 },   // VCO 1296
};
#define SPEEDS (sizeof speeds / sizeof speeds[0])

static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

// Rockchip's clock registers have a "write mask" in the top 16 bits: only
// bits whose mask bit is set change, so no read-modify-write is needed
static void set_bits(uintptr_t a, uint32_t mask, uint32_t value)
{
	wr(a, mask << 16 | value);
}

static void delay_us(uint64_t us)
{
	uint64_t end = READ_SYSREG(cntpct_el0) + us * 24;
	while (READ_SYSREG(cntpct_el0) < end)
		;
}

uint32_t cpu_mhz(void)
{
	uint32_t c0 = rd(APLL_CON(0)), c1 = rd(APLL_CON(1));
	if ((rd(CRU_MODE) & 3) != 1)
		return 24;   // slow mode: the crystal
	uint32_t fb = c0 & 0xfff, p1 = c0 >> 12 & 7, ref = c1 & 0x3f, p2 = c1 >> 6 & 7;
	if (!ref || !p1 || !p2)
		return 0;
	return 24 * fb / ref / p1 / p2;
}

uint32_t cpu_mv(void)
{
	uint32_t mv;
	return rk817_buck_mv(2, &mv) ? 0 : mv;
}

void cpu_speeds(void (*say)(const char *fmt, ...))
{
	for (uint32_t i = 0; i < SPEEDS; i++)
		say("%u MHz at %u mV\n", speeds[i].mhz, speeds[i].mv);
}

static void set_pll(const struct speed *s)
{
	set_bits(CRU_MODE, 3, 0);   // slow: the CPU on the crystal
	__asm__ volatile("dsb sy\nisb");
	set_bits(APLL_CON(0), 0x7fff, s->postdiv1 << 12 | s->fbdiv);
	set_bits(APLL_CON(1), 0x11ff, 1U << 12 | s->postdiv2 << 6 | s->refdiv);   // 1 << 12: whole numbers
	uint64_t give_up = READ_SYSREG(cntpct_el0) + 24000;   // 1 ms
	while (!(rd(APLL_CON(1)) & PLL_LOCK) && READ_SYSREG(cntpct_el0) < give_up)
		;
	set_bits(CRU_MODE, 3, 1);   // back to the PLL
	__asm__ volatile("dsb sy\nisb");
}

const char *cpu_set_mhz(uint32_t mhz)
{
	const struct speed *s = 0;
	for (uint32_t i = 0; i < SPEEDS; i++)
		if (speeds[i].mhz == mhz)
			s = &speeds[i];
	if (!s)
		return "not a speed in the table";
	uint32_t now_mv = cpu_mv();
	if (!now_mv)
		return "can't read vdd_arm from the power chip";

	// The CPU's debug bus: /8, as Linux has it at every speed (at our 400
	// MHz start it's /4, which would be too fast for it at 1.3 GHz)
	set_bits(CLKSEL0, 0xf << 8, 7 << 8);

	if (s->mv > now_mv) {
		if (rk817_buck_set_mv(2, s->mv))
			return "couldn't set vdd_arm";
		// The converter moves at a few mV per microsecond; give it plenty
		delay_us(1000);
	}
	set_pll(s);
	if (cpu_mhz() != mhz)
		return "the PLL didn't take the new setting";   // voltage stays up: safe
	if (s->mv < now_mv)
		rk817_buck_set_mv(2, s->mv);
	return 0;
}

// Count CPU cycles by timing instructions that take exactly one each: a
// chain of adds where every one needs the one before it, so the CPU can't
// do two at once. 64 per loop, so the loop's own instructions hardly count.
uint32_t cpu_measure_mhz(void)
{
	const uint64_t loops = 100000;
	uint64_t t0 = READ_SYSREG(cntpct_el0);
	uint64_t x = 0, n = loops;
	__asm__ volatile(
		"1:\n"
		".rept 64\n"
		"add %0, %0, #1\n"
		".endr\n"
		"subs %1, %1, #1\n"
		"b.ne 1b\n"
		: "+r"(x), "+r"(n));
	uint64_t ticks = READ_SYSREG(cntpct_el0) - t0;   // 24 per microsecond
	return (uint32_t)(loops * 64 * 24 / ticks);
}
