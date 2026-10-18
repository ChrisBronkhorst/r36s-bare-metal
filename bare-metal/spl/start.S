// The SPL's first instructions. The boot ROM loads us at address 0 in RAM
// and calls us like a function (we never return). EL3, MMU and data cache
// off. Rockchip's loader also expects other cores to arrive here, so we
// park any that aren't core 0.

.section .text.boot
.global _start
_start:
	msr	daifset, #0xf		// no interrupts, no SErrors
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbnz	x0, park

	adr	x0, vectors
	msr	vbar_el3, x0
	// SCTLR_EL3: instruction cache on (much faster), data cache and MMU
	// off, no alignment checks
	ldr	x0, =0x30c51830
	msr	sctlr_el3, x0
	msr	cptr_el3, xzr		// floating point not trapped
	isb

	msr	spsel, #1
	adr	x0, __stack_top
	mov	sp, x0

	adr	x0, __bss_start
	adr	x1, __bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b

2:	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	bl	spl_main
park:	wfe
	b	park

// Any exception at all is a crash: report it with flashes
.macro crash
	.balign 0x80
	b	crashed
.endm

.balign 0x800
vectors:
	.rept 16
	crash
	.endr

crashed:
	adr	x0, __stack_top
	mov	sp, x0
	mrs	x0, esr_el3
	mrs	x1, elr_el3
	bl	spl_crash
	b	park
