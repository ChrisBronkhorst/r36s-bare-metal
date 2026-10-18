#pragma once
// What our SPL leaves in memory for the kernel to show, at a fixed address
// just below where the kernel is loaded. (The monitor's firewall covers
// only the first megabyte, so the kernel can read it.)
#include <stdint.h>
#include "sd.h"

#define SPL_REPORT 0x1ff000UL
#define SPL_MAGIC  0x314c505352554fUL   // "OURSPL1" in ASCII

struct spl_report {
	uint64_t magic;
	uint32_t el;                 // the level the boot ROM started us at
	uint32_t failed;             // 0, or the step that failed (= flash count)
	uint64_t ticks_in;           // counter: when we started,
	uint64_t ticks_card;         //   when the card was ready,
	uint64_t ticks_out;          //   when we jumped to the monitor
	uint32_t cru_mode, pmucru_mode, clksel0;   // the clocks the RAM setup left
	uint32_t kernel_addr, kernel_size, kernel_crc, kernel_crc_ok;
	uint32_t monitor_addr, monitor_size;
	uint64_t crash_esr, crash_elr;
	struct sd_info sd;
};
