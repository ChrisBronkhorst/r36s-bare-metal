#pragma once
#include <stdint.h>

// Turn on the MMU and the caches. The framebuffer's memory stays uncached,
// so the display controller (which reads RAM directly, not through our
// caches) still sees every pixel we write. Returns 0 if we can't (not EL2).
int mmu_init(uintptr_t framebuffer, uint32_t size);
