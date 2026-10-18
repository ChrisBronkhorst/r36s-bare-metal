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

// System calls (`svc #n`) come back. These count them.
extern volatile uint32_t syscall_count, last_syscall;
