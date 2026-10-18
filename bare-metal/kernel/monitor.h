#pragma once
#include <stdint.h>

// 1 if our own monitor is at EL3 (not ARM Trusted Firmware)
int monitor_ours(void);
// What's at EL3 and what it did; Y on it calls monitor_peek
void monitor_page(void);
// Try to read the monitor's memory, and say what happened, on this row
void monitor_peek(uint32_t row);
