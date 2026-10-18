/* Stage 1 runs in the chip's own SRAM: the boot ROM loads us at 0xff0e1000
 * (after the 4 KB it uses itself) and calls us like a function. We keep to
 * the same space as Rockchip's 10 KB blob, and like U-Boot's version of
 * this stage, put our stack just above. */

ENTRY(_start)

SECTIONS
{
	. = 0xff0e1000;
	.text   : { KEEP(*(.text.boot)) *(.text*) }
	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }

	. = ALIGN(16);
	__bss_start = .;
	.bss    : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;
	_end = .;
}

__stack_top = 0xff0e4000;
ASSERT(_end <= 0xff0e3800, "stage 1 would run into its stack (2 KB below 0xff0e4000)")
