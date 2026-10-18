#pragma once
#include <stdint.h>

// What can go wrong, as backlight flash counts (after one long flash)
enum { DRAM_ERR_PLL = 1, DRAM_ERR_INIT = 2, DRAM_ERR_TRAINING = 3, DRAM_ERR_TEST = 4 };

int dram_init(void);         // 0, or a DRAM_ERR_
uintptr_t dram_test(void);   // 0, or the first address that failed
void dram_report(void);      // leave the report in RAM for the kernel
uint64_t ticks(void);
void udelay(uint32_t us);

extern struct ddr_report early;
