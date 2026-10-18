// Our EL3 monitor: what runs in ARM Trusted Firmware's place.
//
// Rockchip's loader copies us to 0x10000 (from the card's "trust" image) and
// jumps here with the CPU at EL3, the most privileged level: the secure
// world, where every register and all of memory can be reached. Our job is
// short: set up what only EL3 is allowed to set up, drop to EL2 (the non-
// secure world) at the kernel, and after that only wake up when the kernel
// asks for something with an `smc` instruction (like "reset the board").
//
// x0 = the loader's list of what it loaded (where the kernel starts)
// x1 = a second, platform list (not needed)
// MMU and data cache off, interrupts masked, core 0.

.section .text.boot
.global _start
_start:
	mov	x19, x0
	mov	x20, x1
	mrs	x21, cntpct_el0		// when we started, for the report

	mrs	x0, mpidr_el1		// only core 0 should come here
	and	x0, x0, #0xff
	cbnz	x0, park

	// Our exception vectors first: a crash from here on comes to us
	adr	x0, vectors
	msr	vbar_el3, x0
	isb

	// SCTLR_EL3: MMU and data cache off, instruction cache on, no alignment
	// checks. The fixed "reserved, write 1" bits are 0x30c50830.
	ldr	x0, =0x30c51830
	msr	sctlr_el3, x0
	// CPTR_EL3 = 0: don't trap floating point (or anything else) to us
	msr	cptr_el3, xzr
	isb

	msr	spsel, #1		// use SP_EL3 (not SP_EL0)
	adr	x0, __stack_top
	mov	sp, x0

	// C expects .bss to start as zero
	adr	x0, __bss_start
	adr	x1, __bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b

2:	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	monitor_main		// returns where the kernel starts
	b	enter_kernel

park:	wfe
	b	park

// Drop to EL2 at x0. Called at the end of start-up, and after a crash here.
//   SPSR_EL3 = 0x3c9: go to EL2, using SP_EL2, with D, A, I and F masked
//   ELR_EL3  = where to go
// `eret` then switches level and jumps in one step.
.global enter_kernel
enter_kernel:
	adr	x1, __stack_top		// fresh stack for later `smc`s
	mov	sp, x1
	mov	x1, #0x3c9
	msr	spsr_el3, x1
	msr	elr_el3, x0
	mov	x0, #0			// no device tree
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	isb
	eret

// ---- exception vectors ---------------------------------------------------
// 16 slots of 0x80 bytes: four kinds (sync, IRQ, FIQ, SError) from four
// places (here with SP0, here with SPx, lower level 64-bit, lower level
// 32-bit). Only two matter:
//  - anything from here (EL3) is a crash in our own start-up
//  - sync from a lower level is the kernel's `smc`
// Interrupts and SErrors aren't routed to EL3 (SCR_EL3), so never arrive.

.macro crash slot
	.balign 0x80
	mov	x0, #\slot
	b	crashed
.endm

.macro ignore
	.balign 0x80
	eret
.endm

.balign 0x800
vectors:
	crash 0
	crash 1
	crash 2
	crash 3
	crash 4
	crash 5
	crash 6
	crash 7
	.balign 0x80
	b	smc_entry		// 0x400: sync from a lower level, 64-bit
	ignore
	ignore
	ignore
	ignore				// 0x600: lower level, 32-bit
	ignore
	ignore
	ignore

// Save every register the kernel had (an `smc` must hand them back
// unchanged, except x0-x3, the results), call C, put them back.
smc_entry:
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mov	x0, sp
	bl	smc_handler		// reads x0-x3 from the frame, writes results back
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #256
	eret				// back to the instruction after the `smc`

// A crash during our start-up: note it, then start the kernel anyway, so
// it can show what happened (it asks us with an `smc`).
crashed:
	adr	x1, __stack_top
	mov	sp, x1
	bl	monitor_crashed		// returns where the kernel starts, or never
	b	enter_kernel
