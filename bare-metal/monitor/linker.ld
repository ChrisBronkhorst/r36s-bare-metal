/* Rockchip's loader copies the trust image's BL31 part to 0x10000, where
 * ARM Trusted Firmware lives, and jumps to its first byte. */

ENTRY(_start)

SECTIONS
{
	. = 0x10000;
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
}
