// The MMU: page tables, memory types, and with them the caches.
//
// We use "identity mapping": every virtual address is the same as the
// physical one, so nothing moves. What the tables add is a memory type for
// every region, and the data cache only caches memory marked cacheable.
//
// The address space is 4 GB (32-bit addresses), described with 4 KB-granule
// tables in two levels:
//
//   level 1: 4 entries of 1 GB
//     0x00000000  RAM       -> a level 2 table, to describe it in finer detail
//     0x40000000  nothing   -> invalid: touching it is an exception
//     0x80000000  nothing   -> invalid
//     0xc0000000  hardware  -> one 1 GB "device memory" block (all the
//                              registers live at 0xff000000 and up)
//   level 2: 512 entries of 2 MB, covering the first 1 GB of RAM
//     all cacheable, except the blocks holding the framebuffer

#include "mmu.h"
#include "sysreg.h"

// Memory types. MAIR holds up to 8 type definitions; entries refer to them by
// number (AttrIndx). Ours:
enum { DEVICE = 0, CACHED = 1, UNCACHED = 2 };
#define MAIR  (0x00UL << (8 * DEVICE)     /* Device-nGnRnE: strict, uncached */ \
             | 0xffUL << (8 * CACHED)     /* Normal, write-back cached */      \
             | 0x44UL << (8 * UNCACHED))  /* Normal, not cached */

// Entry bits
#define BLOCK      (1UL << 0)            // a block of memory (bits 1-0 = 01)
#define TABLE      (3UL << 0)            // a pointer to the next table (11)
#define ATTR(type) ((uint64_t)(type) << 2)
#define AP_RW      (1UL << 6)            // read/write (AP[1] must be 1 at EL2)
#define SHAREABLE  (3UL << 8)            // inner shareable: all 4 cores agree
#define AF         (1UL << 10)           // "accessed" flag; 0 would fault
#define XN         (1UL << 54)           // never execute code from here

#define GB (1UL << 30)
#define MB2 (1UL << 21)

// Tables must be aligned to 4 KB. They're in .bss, so they start as zero
// (= invalid entries).
static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));

// SCTLR_EL2 bits
#define SCTLR_M (1UL << 0)    // MMU on
#define SCTLR_A (1UL << 1)    // fault on any misaligned access
#define SCTLR_C (1UL << 2)    // data cache on
#define SCTLR_I (1UL << 12)   // instruction cache on

int mmu_init(uintptr_t framebuffer, uint32_t size)
{
	if (current_el() != 2)
		return 0;

	for (uint64_t i = 0; i < 512; i++) {
		uint64_t addr = i * MB2;
		int screen = addr + MB2 > framebuffer && addr < framebuffer + size;
		level2[i] = addr | BLOCK | ATTR(screen ? UNCACHED : CACHED) | AP_RW | SHAREABLE | AF;
	}
	level1[0] = (uintptr_t)level2 | TABLE;
	level1[3] = 3 * GB | BLOCK | ATTR(DEVICE) | AP_RW | AF | XN;

	// TCR: how to read the tables.
	//   T0SZ = 32: addresses are 64 - 32 = 32 bits (4 GB), so the walk starts
	//              at level 1
	//   IRGN0/ORGN0 = 1, SH0 = 3: the walker may cache the tables themselves
	//   TG0 = 0: 4 KB granule, PS = 0: physical addresses are 32 bits
	//   bits 31 and 23 are "reserved, write 1"
	uint64_t tcr = 1UL << 31 | 1UL << 23 | 3UL << 12 | 1UL << 10 | 1UL << 8 | 32;

	__asm__ volatile(
		"dsb sy\n"                  // tables written to RAM before the walker looks
		"msr mair_el2, %0\n"
		"msr tcr_el2, %1\n"
		"msr ttbr0_el2, %2\n"
		"isb\n"
		"tlbi alle2\n"              // forget any translations cached earlier
		"dsb sy\n"
		"ic iallu\n"                // and any instructions in the cache
		"dsb sy\n"
		"isb\n"
		: : "r"(MAIR), "r"(tcr), "r"((uintptr_t)level1) : "memory");

	uint64_t sctlr = READ_SYSREG(sctlr_el2);
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
	sctlr &= ~SCTLR_A;
	__asm__ volatile("msr sctlr_el2, %0\nisb" : : "r"(sctlr) : "memory");
	return 1;
}
