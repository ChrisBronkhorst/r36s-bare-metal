// The ARM generic timer. The counter (cntpct) counts up 24 million times a
// second, forever. Each core has comparators on it; we use the physical one:
// write a target into CNTP_CVAL, and when the counter passes it, the timer
// raises its interrupt wire (and holds it up until we move the target).

#include "exception.h"
#include "sysreg.h"
#include "timer.h"

volatile uint64_t timer_ticks;
volatile uint64_t timer_late_min = ~0UL, timer_late_max, timer_late_sum;

static uint64_t period, next;

static void timer_interrupt(uint32_t id)
{
	(void)id;
	uint64_t late = READ_SYSREG(cntpct_el0) - next;
	if (late < timer_late_min)
		timer_late_min = late;
	if (late > timer_late_max)
		timer_late_max = late;
	timer_late_sum += late;
	timer_ticks++;

	// Next target: exactly one period after the last one (not after now),
	// so the ticks don't drift however late we were.
	next += period;
	__asm__ volatile("msr cntp_cval_el0, %0\nisb" : : "r"(next));
}

void timer_start(uint32_t hz)
{
	// no interrupts while we change period and next under its feet
	__asm__ volatile("msr daifset, #2" ::: "memory");
	period = READ_SYSREG(cntfrq_el0) / hz;
	next = READ_SYSREG(cntpct_el0) + period;
	__asm__ volatile(
		"msr cntp_cval_el0, %0\n"
		"msr cntp_ctl_el0, %1\n"    // bit 0: enable; bit 1 (mask) left 0
		"isb"
		: : "r"(next), "r"(1UL));
	irq_register(TIMER_IRQ, timer_interrupt, 0x80);
	__asm__ volatile("msr daifclr, #2" ::: "memory");
}
