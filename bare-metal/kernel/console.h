#pragma once
#include <stdint.h>

// A text console on the screen: the 8x8 font drawn at 2x, in 16x20 pixel cells
// (4 pixels of space between lines) with an 8 pixel margin: 39 columns x 23
// rows on the 640x480 screen. Scrolls when it's full.

#define CELL_W 16
#define CELL_H 20
#define MARGIN 8

// the palette
#define BG     0x10202c
#define WHITE  0xf0f0f0
#define GREY   0x8090a0
#define YELLOW 0xf0d020
#define GREEN  0x60d070

void console_init(uint32_t fg, uint32_t bg);   // clears the screen
void console_color(uint32_t fg);
void console_goto(uint32_t col, uint32_t row);
uint32_t console_row(void);
void console_clear_eol(void);   // blank the rest of the current line
void kputc(char c);
void kputs(const char *s);   // no newline added
void kprintf(const char *fmt, ...);   // %s %c %d %u %x %p %%, width like %08x, l for 64-bit

// draw text at a pixel position, outside the console grid
void text_at(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
