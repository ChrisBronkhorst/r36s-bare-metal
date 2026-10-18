#pragma once
// What our RAM setup (stage 1) leaves in RAM for the kernel to show
#include <stdint.h>

#define DDR_REPORT 0x1ff800UL   // 2 KB after the SPL's report, below the kernel
#define DDR_MAGIC  0x3152444452554fUL   // "OURDDR1"

struct ddr_report {
	uint64_t magic;
	uint64_t ticks_in, ticks_out;    // the counter, when we started and finished
	uint32_t stimer_was;             // was the counter already running?
	uint32_t cru_mode_in;            // which PLLs were on when the boot ROM called us
	uint32_t pmucru_mode_in;
	uint32_t apll_con0_in, apll_con1_in;
	uint32_t dpll_lock_us;           // how long the RAM's PLL took to lock
	uint32_t init_us;                // controller + PHY start-up, until the RAM was ready
	uint32_t training_tries;         // gate training attempts (1 = first time)
	uint32_t gates[4];               // training result per byte lane
	uint32_t test_words;             // words checked by the memory test
	uint32_t test_us;
};
