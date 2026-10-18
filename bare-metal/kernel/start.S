// The very first instructions of our kernel.
//
// U-Boot's `booti` command expects the standard 64-byte arm64 "Image" header
// at the front, the same one Linux uses. It checks the magic number, reads
// how big we are, and then jumps to the first word (code0), which branches
// over the rest of the header to real_start.
//
// Two ways to get here:
//  - U-Boot loads us at 0x02000000 (where we're linked) and jumps here with
//    x0 = the device tree.
//  - Without U-Boot, Rockchip's loader puts us where U-Boot would go,
//    0x00200000, and ARM Trusted Firmware jumps here. Then we first copy
//    ourselves to 0x02000000. (The loader insists on U-Boot's address, and
//    our code has addresses baked in for 0x02000000.)
// Either way: MMU and data cache off, interrupts masked, CPU core 0.

.section .text.boot
.global _start
_start:
	b	real_start		// code0: jump over the header
	.long	0			// code1
	.quad	0			// text_offset: run exactly where we were loaded
	.quad	__image_size		// image_size, including .bss and the stack
	.quad	0xa			// flags: little-endian, 4K pages, any 2MB-aligned address
	.quad	0, 0, 0			// reserved
	.long	0x644d5241		// magic: "ARM\x64"
	.long	0			// reserved

real_start:
	mov	x19, x0			// keep the device tree address for kmain

	// only core 0 should get here, but park any other core just in case
	mrs	x1, mpidr_el1
	and	x1, x1, #0xff
	cbnz	x1, park

	// Where are we? `adr` gives the address we're running at, `ldr =`
	// the address the linker gave us. Same: U-Boot loaded us.
	mov	x20, #1			// x20: 1 = U-Boot started us
	adr	x1, _start
	ldr	x2, =_start
	cmp	x1, x2
	b.eq	3f

	// Copy code and data (not .bss, that's cleared below) to where we belong.
	mov	x20, #0
	mov	x19, #0			// no device tree without U-Boot
	ldr	x3, =__bss_start
	sub	x3, x3, x2		// bytes to copy, a multiple of 16
	mov	x4, x2
1:	ldp	x5, x6, [x1], #16
	stp	x5, x6, [x4], #16
	subs	x3, x3, #16
	b.gt	1b
	// The instruction cache may still hold whatever was at the new address:
	// throw it away, then continue in the copy.
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	ldr	x1, =3f
	br	x1
3:

	// C expects .bss (uninitialized globals) to start as zero
	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b

	// a stack, so we can call C functions
2:	adrp	x1, __stack_top
	add	x1, x1, :lo12:__stack_top
	mov	sp, x1

	mov	x0, x19
	mov	x1, x20
	bl	kmain

park:	wfe				// sleep until an event, forever
	b	park
