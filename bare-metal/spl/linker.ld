/* The boot ROM loads us at address 0 in RAM. The monitor will go to
 * 0x10000, so we (code, data and stack) must end before that. */

ENTRY(_start)

SECTIONS
{
	. = 0x0;
	.text   : { KEEP(*(.text.boot)) *(.text*) }
	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }

	. = ALIGN(16);
	__bss_start = .;
	.bss    : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;

	. = . + 0x2000;		/* 8 KB stack */
	__stack_top = .;
	_end = .;
}

ASSERT(_end <= 0x10000, "the SPL would overlap the monitor at 0x10000")
