// The other three cores.
//
// Core 0 asks the monitor to start each one (PSCI CPU_ON, an `smc`). A new
// core arrives at secondary_entry (start.S) at EL2 with its caches and MMU
// off, turns them on with core 0's settings, and comes to core_main below,
// where it waits for jobs, forever.
//
// A job: core 0 writes the function and its argument, then bumps job_seq
// and wakes the core with `sev` ("send event", which ends a `wfe` on every
// core). The core runs it, sets done_seq = job_seq, and does `sev` too.
// Each field has one writer, so no locks are needed, only "barriers":
// `dmb` makes sure the writes before it are seen by other cores before the
// writes after it (otherwise the CPU and memory system may reorder them,
// and a core could see the new job_seq but the old function).

#include <stdint.h>
#include "cores.h"
#include "exception.h"
#include "mmu.h"

#define PSCI_CPU_ON 0xc4000003
#define PSCI_CPU_OFF 0x84000002
#define GRF_CPU_STATUS1 0xff140524UL   // bits 4-7: that core is in wfi

static struct core {
	volatile uint32_t alive;
	volatile uint64_t job_seq, done_seq, jobs;
	void (*volatile fn)(void *arg);
	void *volatile arg;
} cores[4] __attribute__((aligned(64)));

uint8_t core_stacks[3][16384] __attribute__((aligned(16)));   // cores 1-3
void secondary_entry(uint64_t core);   // start.S

static uint64_t smc4(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			 "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

// A new core, MMU and caches on (start.S), interrupts masked
void cores_off_one(void)
{
	smc4(PSCI_CPU_OFF, 0, 0, 0);
}

void core_main(uint32_t c)
{
	vectors_install();   // VBAR is per core: crashes here go to our handler
	struct core *me = &cores[c];
	uint64_t seen = me->job_seq;
	__asm__ volatile("dmb ish" ::: "memory");
	me->alive = 1;
	__asm__ volatile("dsb ish\nsev" ::: "memory");
	for (;;) {
		while (me->job_seq == seen)
			__asm__ volatile("wfe" ::: "memory");
		seen = me->job_seq;
		__asm__ volatile("dmb ish" ::: "memory");   // the job's details, after its number
		me->fn(me->arg);
		me->jobs++;
		__asm__ volatile("dmb ish" ::: "memory");   // its results, before "done"
		me->done_seq = seen;
		__asm__ volatile("dsb ish\nsev" ::: "memory");
	}
}

int core_alive(uint32_t c) { return c < 4 && (c == 0 || cores[c].alive); }
uint64_t core_jobs(uint32_t c) { return c < 4 ? cores[c].jobs : 0; }

const char *cores_start(void)
{
	mmu_publish();   // the settings a new core reads before its caches are on
	for (uint32_t c = 1; c < 4; c++) {
		if (cores[c].alive)
			continue;
		uint64_t r = smc4(PSCI_CPU_ON, c, (uintptr_t)secondary_entry, c);
		if (r == ~0UL)
			return "the monitor doesn't know CPU_ON (an older one: install the card)";
		if (r != 0 && r != ~0UL - 3)   // -4: already on
			return "the monitor couldn't start a core";
		uint64_t t0, t;
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
		do {
			__asm__ volatile("wfe\nmrs %0, cntpct_el0" : "=r"(t) :: "memory");
			if (t - t0 > 24000 * 100)
				return "a core didn't arrive (100 ms)";
		} while (!cores[c].alive);
	}
	return 0;
}

void core_run(uint32_t c, void (*fn)(void *arg), void *arg)
{
	cores[c].fn = fn;
	cores[c].arg = arg;
	__asm__ volatile("dmb ish" ::: "memory");   // the details before the number
	cores[c].job_seq++;
	__asm__ volatile("dsb ish\nsev" ::: "memory");
}

// Hand cores 1-3 back to the monitor before a new kernel replaces us. Left
// in core_main, they'd run the new kernel's bytes as if they were ours.
static void off_job(void *unused)
{
	(void)unused;
	cores_off_one();
}
void cores_stop(void)
{
	for (uint32_t c = 1; c < 4; c++) {
		if (!cores[c].alive || core_busy(c))
			continue;   // (a busy core: nothing we can do but go on)
		cores[c].alive = 0;
		core_run(c, off_job, 0);
		uint64_t t0, t;
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
		do   // asleep in the monitor
			__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t) :: "memory");
		while (!(*(volatile uint32_t *)GRF_CPU_STATUS1 & 1U << (4 + c)) && t - t0 < 24000 * 10);
	}
}

int core_busy(uint32_t c)
{
	return cores[c].done_seq != cores[c].job_seq;
}

void core_wait(uint32_t c)
{
	while (cores[c].done_seq != cores[c].job_seq)
		__asm__ volatile("wfe" ::: "memory");
	__asm__ volatile("dmb ish" ::: "memory");   // then its results
}
