// The screen. We don't set up the display ourselves (yet): U-Boot already did
// that to show its logo, and the display controller (the "VOP") keeps sending
// a block of memory, the framebuffer, to the LCD about 60 times a second. So
// we ask the VOP where that memory is and what pixel format it uses, then
// write our own pixels into it.

#include "screen.h"

// Display controllers (from the RK3326/PX30 device tree). VOPB is the "big"
// one that normally drives the LCD; VOPL is the "lite" one.
#define VOPB 0xff460000UL
#define VOPL 0xff470000UL

// VOP register offsets (from Linux's rockchip_vop_reg.h, PX30 section)
#define WIN0_CTRL0    0x50   // bit 0 enable, bits 1-3 pixel format
#define WIN0_VIR      0x5c   // bytes per line / 4
#define WIN0_MST      0x60   // framebuffer address
#define WIN0_ACT_INFO 0x68   // (height-1) << 16 | (width-1)
#define WIN1_CTRL0    0x90   // bit 0 enable, bits 4-6 pixel format
#define WIN1_VIR      0x98
#define WIN1_MST      0xa0
#define WIN1_DSP_INFO 0xa4   // (height-1) << 16 | (width-1)

struct screen screen;

static inline uint32_t read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

// Look at one VOP's windows and use the first one that's switched on.
static int find_window(uintptr_t vop)
{
	uint32_t ctrl = read32(vop + WIN0_CTRL0);
	uint32_t size;
	if (vop == VOPB && (ctrl & 1)) {
		screen.window = 0;
		screen.format = (ctrl >> 1) & 7;
		screen.fb = (volatile uint8_t *)(uintptr_t)read32(vop + WIN0_MST);
		screen.pitch = (read32(vop + WIN0_VIR) & 0x1fff) * 4;
		size = read32(vop + WIN0_ACT_INFO);
	} else if ((ctrl = read32(vop + WIN1_CTRL0)) & 1) {
		screen.window = 1;
		screen.format = (ctrl >> 4) & 7;
		screen.fb = (volatile uint8_t *)(uintptr_t)read32(vop + WIN1_MST);
		screen.pitch = (read32(vop + WIN1_VIR) & 0x1fff) * 4;
		size = read32(vop + WIN1_DSP_INFO);
	} else {
		return 0;
	}
	screen.vop = vop;
	screen.width = (size & 0x1fff) + 1;
	screen.height = ((size >> 16) & 0x1fff) + 1;
	return screen.fb != 0 && screen.format <= FMT_RGB565;
}

int screen_init(void)
{
	return find_window(VOPB) || find_window(VOPL);
}

const char *format_name(uint32_t format)
{
	switch (format) {
	case FMT_ARGB8888: return "ARGB8888";
	case FMT_RGB888:   return "RGB888";
	case FMT_RGB565:   return "RGB565";
	}
	return "unknown";
}

// With the MMU off, every memory access is "device" memory, so each write
// must be aligned to its own size. That's why the 3-byte format writes
// byte by byte.
void pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
	if (x >= screen.width || y >= screen.height)
		return;
	uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
	volatile uint8_t *p = screen.fb + y * screen.pitch;
	switch (screen.format) {
	case FMT_ARGB8888:
		*(volatile uint32_t *)(p + x * 4) = 0xff000000 | rgb;
		break;
	case FMT_RGB888:
		p += x * 3;
		p[0] = b; p[1] = g; p[2] = r;
		break;
	case FMT_RGB565:
		*(volatile uint16_t *)(p + x * 2) =
			(uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		break;
	}
}

void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
	for (uint32_t j = y; j < y + h; j++)
		for (uint32_t i = x; i < x + w; i++)
			pixel(i, j, rgb);
}

void scroll_up(uint32_t lines, uint32_t rgb)
{
	if (lines >= screen.height) {
		rect(0, 0, screen.width, screen.height, rgb);
		return;
	}
	// the pitch is always a multiple of 4, so copy 4 bytes at a time
	uint32_t words = (screen.height - lines) * screen.pitch / 4;
	volatile uint32_t *dst = (volatile uint32_t *)screen.fb;
	volatile uint32_t *src = (volatile uint32_t *)(screen.fb + lines * screen.pitch);
	for (uint32_t i = 0; i < words; i++)
		dst[i] = src[i];
	rect(0, screen.height - lines, screen.width, lines, rgb);
}
