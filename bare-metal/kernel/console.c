#include <stdarg.h>
#include "console.h"
#include "screen.h"

extern const uint8_t font8x8[128][8];

static uint32_t col, row, cols, rows;
static uint32_t fg, bg;

void console_init(uint32_t fg_rgb, uint32_t bg_rgb)
{
	fg = fg_rgb;
	bg = bg_rgb;
	cols = (screen.width - 2 * MARGIN) / CELL_W;
	rows = (screen.height - 2 * MARGIN) / CELL_H;
	col = row = 0;
	rect(0, 0, screen.width, screen.height, bg);
}

void console_color(uint32_t fg_rgb)
{
	fg = fg_rgb;
}

void console_goto(uint32_t c, uint32_t r)
{
	col = c;
	row = r;
}

uint32_t console_row(void)
{
	return row;
}

// Each glyph row is one byte; bit 0 is the leftmost pixel. Every font pixel
// becomes a 2x2 block on screen.
static void draw_glyph(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
	const uint8_t *glyph = font8x8[(uint8_t)c & 0x7f];
	for (uint32_t gy = 0; gy < 8; gy++)
		for (uint32_t gx = 0; gx < 8; gx++) {
			uint32_t rgb = (glyph[gy] >> gx) & 1 ? fg : bg;
			pixel(x + gx * 2, y + gy * 2, rgb);
			pixel(x + gx * 2 + 1, y + gy * 2, rgb);
			pixel(x + gx * 2, y + gy * 2 + 1, rgb);
			pixel(x + gx * 2 + 1, y + gy * 2 + 1, rgb);
		}
}

static void newline(void)
{
	col = 0;
	if (++row >= rows) {
		scroll_up(CELL_H, bg);
		row = rows - 1;
	}
}

void kputc(char c)
{
	if (c == '\n') {
		newline();
		return;
	}
	if (c == '\r') {
		col = 0;
		return;
	}
	if (col >= cols)
		newline();
	draw_glyph(c, MARGIN + col * CELL_W, MARGIN + row * CELL_H, fg, bg);
	col++;
}

void kputs(const char *s)
{
	while (*s)
		kputc(*s++);
}

void text_at(uint32_t x, uint32_t y, const char *s, uint32_t fg_rgb, uint32_t bg_rgb)
{
	for (; *s; s++, x += CELL_W)
		draw_glyph(*s, x, y, fg_rgb, bg_rgb);
}

static void put_number(uint64_t n, uint32_t base, uint32_t width, char pad, int negative)
{
	char digits[24];
	int len = 0;
	do {
		digits[len++] = "0123456789abcdef"[n % base];
		n /= base;
	} while (n);
	if (negative)
		width = width ? width - 1 : 0;
	if (negative && pad == '0')
		kputc('-');
	for (uint32_t i = len; i < width; i++)
		kputc(pad);
	if (negative && pad != '0')
		kputc('-');
	while (len)
		kputc(digits[--len]);
}

void kprintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			kputc(*fmt);
			continue;
		}
		fmt++;
		char pad = ' ';
		uint32_t width = 0;
		int is_long = 0;
		if (*fmt == '0')
			pad = '0';
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l') {
			is_long = 1;
			fmt++;
		}
		switch (*fmt) {
		case 's': kputs(va_arg(args, const char *)); break;
		case 'c': kputc((char)va_arg(args, int)); break;
		case 'd': {
			int64_t n = is_long ? va_arg(args, int64_t) : va_arg(args, int);
			put_number(n < 0 ? -(uint64_t)n : (uint64_t)n, 10, width, pad, n < 0);
			break;
		}
		case 'u':
			put_number(is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t), 10, width, pad, 0);
			break;
		case 'x':
			put_number(is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t), 16, width, pad, 0);
			break;
		case 'p':
			kputs("0x");
			put_number((uintptr_t)va_arg(args, void *), 16, width, pad, 0);
			break;
		case '%': kputc('%'); break;
		case '\0': fmt--; break;   // a lone % at the very end
		default: kputc('%'); kputc(*fmt); break;
		}
	}
	va_end(args);
}
