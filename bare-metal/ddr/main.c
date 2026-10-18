// Our stage 1: the RAM setup, in place of Rockchip's DDR blob.
//
// The boot ROM, built into the chip, found our image at the card's sector
// 64 and loaded this part into the chip's own SRAM (there's no RAM yet: that's
// our job). When we return, it loads stage 2, our SPL, into the RAM we just
// brought up, and runs it.
//
// If something fails, the backlight shows it: one long flash, then the
// failure's number in short ones (dram.h), over and over. Rockchip's blob
// printed to a serial port instead; we have none connected.

#include <stdint.h>
#include "backlight.h"
#include "board.h"
#include "dram.h"
#include "report.h"

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

// The CPU's counter (cntpct, which udelay uses) is driven by secure timer 1.
// If the boot ROM hasn't started it, we do, as U-Boot's version of this stage
// does: count up from 0, free-running.
static void counter_start(void)
{
	uintptr_t ch1 = 0xff220020;
	early.stimer_was = rd(ch1 + 0x10);
	if (early.stimer_was & 1)
		return;
	wr(ch1 + 0x10, 0);
	wr(ch1 + 0x00, 0xffffffff);
	wr(ch1 + 0x04, 0xffffffff);
	wr(ch1 + 0x10, 1);
}

static void __attribute__((noreturn)) fail(uint32_t code)
{
	board_init();
	for (;;) {
		backlight_set(100);
		udelay(1500000);
		backlight_set(0);
		udelay(600000);
		for (uint32_t i = 0; i < code; i++) {
			backlight_set(100);
			udelay(250000);
			backlight_set(0);
			udelay(350000);
		}
		udelay(2000000);
	}
}

void ddr_main(void)
{
	counter_start();
	early.ticks_in = ticks();
	// What the boot ROM set up before us: are the CPU's and the general
	// PLLs already on? (Rockchip's blob left both on.)
	early.cru_mode_in = rd(0xff2b00a0);
	early.pmucru_mode_in = rd(0xff2bc020);
	early.apll_con0_in = rd(0xff2b0000);
	early.apll_con1_in = rd(0xff2b0004);

	int err = dram_init();
	if (err)
		fail(err);
	uint64_t t = ticks();
	if (dram_test())
		fail(DRAM_ERR_TEST);
	early.test_us = (ticks() - t) / 24;
	dram_report();
}

// The compiler may call these for struct copies; there's no C library
void *memset(void *dst, int c, unsigned long n)
{
	volatile uint8_t *d = dst;
	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	volatile uint8_t *d = dst;
	const volatile uint8_t *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}
