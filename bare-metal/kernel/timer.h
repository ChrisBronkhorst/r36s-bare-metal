#pragma once
#include <stdint.h>

// The CPU's own timer, firing an interrupt `hz` times a second.

#define TIMER_IRQ 30   // the non-secure physical timer: PPI 14, 16 + 14

void timer_start(uint32_t hz);
void timer_interrupt(void);   // called by the IRQ handler

extern volatile uint64_t timer_ticks;   // interrupts so far

// How late the handler ran after the timer went off, in counter ticks
// (24 MHz: one tick is 41.7 ns)
extern volatile uint64_t timer_late_min, timer_late_max, timer_late_sum;
