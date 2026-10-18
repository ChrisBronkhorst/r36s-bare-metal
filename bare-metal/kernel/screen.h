#pragma once
#include <stdint.h>

struct screen {
	volatile uint8_t *fb;   // framebuffer: the memory the LCD shows
	uint32_t width, height;
	uint32_t pitch;         // bytes from one line to the next
	uint32_t format;        // FMT_ARGB8888, FMT_RGB888 or FMT_RGB565
	uintptr_t vop;          // which display controller is driving it
	uint32_t window;        // and which of its windows (layers)
};

#define FMT_ARGB8888 0
#define FMT_RGB888   1
#define FMT_RGB565   2

extern struct screen screen;

int screen_init(void);   // find the framebuffer U-Boot left running; 0 if none
void pixel(uint32_t x, uint32_t y, uint32_t rgb);
void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void scroll_up(uint32_t lines, uint32_t rgb);   // move the picture up, fill the gap
const char *format_name(uint32_t format);
