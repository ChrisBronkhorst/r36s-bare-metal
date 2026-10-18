// The very first instructions of our kernel.
//
// U-Boot's `booti` command expects the standard 64-byte arm64 "Image" header
// at the front, the same one Linux uses. It checks the magic number, reads
// how big we are, and then jumps to the first word (code0), which branches
// over the rest of the header to real_start.
//
// When we get here: the MMU and caches are off, interrupts are masked,
// x0 holds the address of the device tree, and we are on CPU core 0.

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
	bl	kmain

park:	wfe				// sleep until an event, forever
	b	park
