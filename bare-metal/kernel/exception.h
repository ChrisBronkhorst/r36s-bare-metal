#pragma once
#include <stdint.h>

// The registers as they were when the exception happened, saved by vectors.S.
struct regs {
	uint64_t x[31];   // x0-x30 (x30 is the return address of the current call)
	uint64_t sp;
};

void vectors_install(void);   // vectors.S: point VBAR at our table

// Called by vectors.S for every exception. Returns only for system calls.
void exception(uint64_t slot, struct regs *r);

// Run handler(id) whenever interrupt `id` arrives (and enable it in the GIC).
void irq_register(uint32_t id, void (*handler)(uint32_t id), uint8_t priority);
extern volatile uint32_t irq_current;   // the interrupt being handled, or 1023

// Reset the whole board, through ARM Trusted Firmware. Doesn't return.
void restart(void);

// While `survive_faults` is set, a data abort doesn't stop us: the first
// one is recorded here, the faulting instruction is skipped, and we carry
// on. For bring-up without a screen, where a crash screen can't be seen.
extern volatile int survive_faults;
extern volatile uint64_t fault_count, fault_esr, fault_far, fault_elr;

// System calls (`svc #n`) come back. These count them.
extern volatile uint32_t syscall_count, last_syscall;
