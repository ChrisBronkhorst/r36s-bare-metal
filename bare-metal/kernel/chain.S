// chainload(entry): leave this kernel for another one, already in memory.
//
// The new kernel expects what the SPL gives it: MMU and caches off,
// interrupts masked, x0 = 0. Turning the caches off is the delicate part.
// The data cache still holds things only it knows (the new kernel we just
// received, for one), so every dirty line must be written back to RAM. And
// it must also forget everything, or stale lines of ours could be written
// back later, over the new kernel, once it turns its own caches on.
//
// So: stop the cache taking new lines (SCTLR.C = 0), then clean and
// invalidate it by "set/way" (every line of every level, by position, the
// same loop Linux uses), then turn the MMU off and jump. Only registers from
// here on: the stack is in RAM, and its newest values may sit in the cache
// we're throwing away.

.global chainload
chainload:
	msr	daifset, #0xf		// no interrupts of any kind
	mov	x17, x0			// the entry point

	mrs	x1, sctlr_el2
	bic	x1, x1, #(1 << 2)	// C: data cache off
	msr	sctlr_el2, x1
	isb

	// Clean and invalidate every data/unified cache level up to the
	// "level of coherence" (where everyone sees the same RAM)
	mrs	x0, clidr_el1
	and	x3, x0, #0x7000000
	lsr	x3, x3, #23		// x3 = LoC x 2
	cbz	x3, 5f
	mov	x10, #0			// x10 = level x 2 (what CSSELR wants)
1:	add	x2, x10, x10, lsr #1	// level x 3: this level's 3 bits in CLIDR
	lsr	x1, x0, x2
	and	x1, x1, #7
	cmp	x1, #2
	b.lt	4f			// no data cache at this level
	msr	csselr_el1, x10		// pick the level, then read its shape
	isb
	mrs	x1, ccsidr_el1
	and	x2, x1, #7
	add	x2, x2, #4		// x2 = log2(line size in bytes)
	mov	x4, #0x3ff
	and	x4, x4, x1, lsr #3	// x4 = ways - 1
	clz	w5, w4			// x5 = where the way number goes
	mov	x7, #0x7fff
	and	x7, x7, x1, lsr #13	// x7 = sets - 1
2:	mov	x9, x4			// for each set...
3:	lsl	x6, x9, x5		// ...and each way
	orr	x11, x10, x6
	lsl	x6, x7, x2
	orr	x11, x11, x6
	dc	cisw, x11		// clean and invalidate that line
	subs	x9, x9, #1
	b.ge	3b
	subs	x7, x7, #1
	b.ge	2b
4:	add	x10, x10, #2
	cmp	x3, x10
	b.gt	1b
5:	mov	x10, #0
	msr	csselr_el1, x10
	dsb	sy
	isb

	// MMU and instruction cache off, and forget cached translations and
	// instructions
	mrs	x1, sctlr_el2
	bic	x1, x1, #(1 << 0)	// M
	bic	x1, x1, #(1 << 12)	// I
	msr	sctlr_el2, x1
	isb
	ic	iallu
	tlbi	alle2
	dsb	sy
	isb

	mov	x0, #0			// no device tree, as from the SPL
	br	x17
