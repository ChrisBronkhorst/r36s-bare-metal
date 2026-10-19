// The exception vector table.
//
// When something interrupts normal execution (a crash, a system call, an
// interrupt) the CPU saves where it was in ELR and its status in SPSR, masks
// interrupts, and jumps to VBAR + an offset that depends on what happened:
//
//                          sync    IRQ     FIQ     SError
//   same level, SP0        0x000   0x080   0x100   0x180
//   same level, SPx        0x200   0x280   0x300   0x380   <- us
//   lower level, 64-bit    0x400   0x480   0x500   0x580
//   lower level, 32-bit    0x600   0x680   0x700   0x780
//
// Each slot is 128 bytes of code. Ours all do the same thing: save the
// registers, and call exception(slot number, saved registers) in C.
// If that returns, put the registers back and `eret` to where we were.
//
// ELR and SPSR are saved too. An exception inside an exception (a bad
// address survived inside an interrupt handler, say) overwrites them, and
// then the outer one would "return" to the inner one's place instead.

#define FRAME 272	// x0-x30, sp, ELR and SPSR, 8 bytes each

	.text
	.balign	0x800		// the table's address must be a multiple of 2 KB
	.global	vectors
vectors:
	.irp	slot, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	.balign	0x80
	sub	sp, sp, #FRAME
	stp	x0, x1, [sp, #0]
	mov	x0, #\slot
	b	exception_entry
	.endr

exception_entry:
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
	add	x1, sp, #FRAME		// sp as it was before the exception
	stp	x30, x1, [sp, #240]
	mrs	x1, CurrentEL		// ELR and SPSR: the EL2 or EL1 ones
	cmp	x1, #(2 << 2)
	b.ne	1f
	mrs	x2, elr_el2
	mrs	x3, spsr_el2
	b	2f
1:	mrs	x2, elr_el1
	mrs	x3, spsr_el1
2:	stp	x2, x3, [sp, #256]

	mov	x1, sp			// x0 = slot, x1 = the saved registers
	bl	exception

	ldp	x2, x3, [sp, #256]	// as saved (exception() may have moved ELR on,
	mrs	x1, CurrentEL		// past a faulting instruction)
	cmp	x1, #(2 << 2)
	b.ne	3f
	msr	elr_el2, x2
	msr	spsr_el2, x3
	b	4f
3:	msr	elr_el1, x2
	msr	spsr_el1, x3
4:	ldp	x2, x3, [sp, #16]
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
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #FRAME
	eret				// back to ELR, with the status from SPSR

// Point the CPU at the table. VBAR exists once per level, so pick ours.
	.global	vectors_install
vectors_install:
	adrp	x0, vectors
	add	x0, x0, :lo12:vectors
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	1f
	msr	vbar_el2, x0
	b	2f
1:	msr	vbar_el1, x0
2:	isb				// make sure the change has taken effect
	ret
