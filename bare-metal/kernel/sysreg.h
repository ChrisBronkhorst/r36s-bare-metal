#pragma once
#include <stdint.h>

// Read one of the CPU's system registers (they have names, not addresses).
#define READ_SYSREG(name) ({ uint64_t v; __asm__ volatile("mrs %0, " #name : "=r"(v)); v; })

static inline uint32_t current_el(void)
{
	return READ_SYSREG(CurrentEL) >> 2 & 3;
}
