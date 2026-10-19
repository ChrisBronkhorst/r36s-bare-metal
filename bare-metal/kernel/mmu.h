#pragma once
#include <stdint.h>

// Turn on the MMU and the caches. The framebuffer's memory stays uncached,
// so the display controller (which reads RAM directly, not through our
// caches) still sees every pixel we write. Returns 0 if we can't (not EL2).
int mmu_init(uintptr_t framebuffer, uint32_t size);

// The same settings, for the other cores to switch on with (start.S reads
// them before its caches are on, so mmu_publish writes them out to RAM)
struct mmu_regs { uint64_t mair, tcr, ttbr, sctlr; };
extern struct mmu_regs mmu_regs;
void mmu_publish(void);
