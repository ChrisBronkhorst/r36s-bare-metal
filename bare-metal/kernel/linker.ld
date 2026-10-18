/* Where everything goes in memory. U-Boot loads kernel.img at 0x02000000
 * (the same place it loads Linux), so we link the code to run there. */

ENTRY(_start)

SECTIONS
{
	. = 0x02000000;
	.text   : { KEEP(*(.text.boot)) *(.text*) }
	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }

	. = ALIGN(16);
	__bss_start = .;
	.bss    : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;

	. = . + 0x4000;		/* 16 KB stack, growing down from __stack_top */
	__stack_top = .;
	_end = .;
}

__image_size = _end - _start;
