#pragma once
// The CPU's clock speed, and the voltage that goes with it
#include <stdint.h>

// Set the CPU to one of the speeds in cpufreq.c's table (MHz). Returns 0,
// or what went wrong (nothing is changed then).
const char *cpu_set_mhz(uint32_t mhz);
uint32_t cpu_mhz(void);             // as the PLL is set now
uint32_t cpu_mv(void);              // vdd_arm as the power chip is set, 0 if unreadable
uint32_t cpu_measure_mhz(void);     // timed: one-cycle instructions per second
void cpu_speeds(void (*say)(const char *fmt, ...));   // the table
