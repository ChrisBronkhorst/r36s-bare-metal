// Stage 1's entry. The boot ROM checks for "RK33" at the start, then calls
// the next word like a C function: when we return 0, it goes on to load
// stage 2 (our SPL) into the now-working RAM. So unlike every later stage,
// we must hand back the ROM's registers and stack exactly as we got them.

.section .text.boot
.global _start
_start:
	.ascii	"RK33"
entry:
	// Save the registers C code may not change, on the ROM's own stack
	stp	x19, x20, [sp, #-32]!
	stp	x29, x30, [sp, #16]
	mov	x19, sp			// remember the ROM's stack...
	adr	x0, __stack_top
	mov	sp, x0			// ...and use our own

	adr	x0, __bss_start
	adr	x1, __bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b

2:	bl	ddr_main		// returns only on success

	mov	sp, x19
	ldp	x29, x30, [sp, #16]
	ldp	x19, x20, [sp], #32
	mov	x0, #0			// 0: "carry on, load the next stage"
	ret
