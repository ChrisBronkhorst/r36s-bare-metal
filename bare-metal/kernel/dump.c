// The display pipeline, from the memory to the glass:
//
//   framebuffer in RAM -> VOP -> DSI host -> video PHY -> 4 wire pairs -> panel
//
// The VOP ("video output processor") reads the framebuffer and produces
// pixels at the pixel clock, with the blank gaps (porches, syncs) that the
// panel expects around each line and each frame. The DSI host packs those
// pixels into MIPI DSI packets. The PHY turns the packets into fast
// differential signals on 4 data lanes + 1 clock lane.
//
// Register layouts from Linux: rockchip_vop_reg.c (PX30), dw-mipi-dsi.c
// (the DSI host is Synopsys DesignWare IP), phy-rockchip-inno-dsidphy.c
// (the PHY is Innosilicon IP) and clk-px30.c.

#include "console.h"
#include "dump.h"
#include "rk817.h"

#define CRU     0xff2b0000UL   // clocks and resets
#define PMUCRU  0xff2bc000UL   // the always-on part of the clocks
#define GRF     0xff140000UL   // "general register file": odds and ends
#define PHY     0xff2e0000UL
#define DSI     0xff450000UL
#define VOP     0xff460000UL
#define GPIO3   0xff270000UL

#define CLKSEL(n)  (CRU + 0x100 + 4 * (n))
#define CLKGATE(n) (CRU + 0x200 + 4 * (n))
#define PANEL_RESET 16   // GPIO3 pin 16 (C0), low = held in reset

static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

// A PHY register: 8 parts (analog, digital, each lane, LVDS) of 32 bytes,
// one byte per 32-bit word
static uint32_t phy(uint32_t part, uint32_t reg) { return rd(PHY + ((part << 5 | reg) << 2)) & 0xff; }

static void name(const char *s)
{
	console_color(GREY);
	uint32_t n = 0;
	for (; s[n]; n++)
		kputc(s[n]);
	for (; n < 8; n++)
		kputc(' ');
	console_color(WHITE);
}

// ---- clocks -----------------------------------------------------------------

// A Rockchip PLL multiplies the 24 MHz crystal:
//   out = 24 MHz / refdiv * (fbdiv + frac / 2^24) / postdiv1 / postdiv2
// In "slow" mode it's bypassed and the output is the crystal itself.
static uint32_t pll_khz(uintptr_t con, uint32_t mode)
{
	if (mode == 0)
		return 24000;
	if (mode != 1)
		return 32;   // "deep slow": a 32 kHz clock
	uint32_t c0 = rd(con), c1 = rd(con + 4), c2 = rd(con + 8);
	uint64_t fb = c0 & 0xfff, post1 = c0 >> 12 & 7;
	uint64_t ref = c1 & 0x3f, post2 = c1 >> 6 & 7;
	uint64_t frac = c1 >> 12 & 1 ? 0 : c2 & 0xffffff;   // DSMPD 1: whole numbers only
	if (!ref || !post1 || !post2)
		return 0;
	return (24000ULL * fb + (24000ULL * frac >> 24)) / ref / post1 / post2;
}

uint32_t dpll_khz(void)
{
	return pll_khz(CRU + 0x20, rd(CRU + 0xa0) >> 4 & 3);
}

uint32_t gpll_khz(void)
{
	return pll_khz(PMUCRU + 0x00, rd(PMUCRU + 0x20) & 3);
}

struct clocks { uint32_t cpll, npll, gpll, dclk, src, div, mux, num, den; };

static void read_clocks(struct clocks *c)
{
	uint32_t mode = rd(CRU + 0xa0);
	c->cpll = pll_khz(CRU + 0x40, mode >> 2 & 3);
	c->npll = pll_khz(CRU + 0x60, mode >> 6 & 3);
	c->gpll = gpll_khz();

	// The VOP's pixel clock, "dclk_vopb": CPLL or NPLL, divided by 1-256,
	// then optionally through a fractional divider (num/den)
	uint32_t sel5 = rd(CLKSEL(5)), sel6 = rd(CLKSEL(6));
	c->src = sel5 >> 11 & 1;
	c->div = (sel5 & 0xff) + 1;
	c->mux = sel5 >> 14 & 3;   // 0 divided, 1 fractional, 2 the crystal
	c->num = sel6 >> 16;
	c->den = sel6 & 0xffff;
	uint32_t khz = (c->src ? c->npll : c->cpll) / c->div;
	c->dclk = c->mux == 0 ? khz :
		c->mux == 1 ? (c->den ? (uint64_t)khz * c->num / c->den : 0) : 24000;
}

static void mhz(uint32_t khz)
{
	kprintf("%u.%03u", khz / 1000, khz % 1000);
}

// ---- page 0: the summary ------------------------------------------------------

static void summary(void)
{
	struct clocks c;
	read_clocks(&c);
	name("PLLs");
	kprintf("C %u  N %u  G %u MHz\n", c.cpll / 1000, c.npll / 1000, c.gpll / 1000);
	name("Pixels");
	mhz(c.dclk);
	kprintf(" MHz: %s/%u%s\n", c.src ? "NPLL" : "CPLL", c.div,
		c.mux == 1 ? " frac" : c.mux == 2 ? " (24M!)" : "");

	// The VOP's timing registers hold, for each direction:
	//   total length, sync length, and where the visible part starts and ends.
	// Everything is counted from the start of the sync pulse.
	uint32_t ht = rd(VOP + 0x100), ha = rd(VOP + 0x104);
	uint32_t vt = rd(VOP + 0x108), va = rd(VOP + 0x10c);
	uint32_t htotal = ht >> 16 & 0xfff, hsync = ht & 0xfff;
	uint32_t hst = ha >> 16 & 0xfff, hend = ha & 0xfff;
	uint32_t vtotal = vt >> 16 & 0xfff, vsync = vt & 0xfff;
	uint32_t vst = va >> 16 & 0xfff, vend = va & 0xfff;
	uint32_t hz10 = htotal && vtotal ? (uint64_t)c.dclk * 10000 / htotal / vtotal : 0;
	name("Mode");
	kprintf("%ux%u, %u.%u frames/s\n", hend - hst, vend - vst, hz10 / 10, hz10 % 10);
	name("Line");   // in pixels
	kprintf("%u: sync %u back %u fr %u\n", htotal, hsync, hst - hsync, htotal - hend);
	name("Frame");  // in lines
	kprintf("%u: sync %u back %u fr %u\n", vtotal, vsync, vst - vsync, vtotal - vend);

	// The VOP can stack 3 windows (layers). U-Boot uses window 1, a
	// simpler one without scaling, and leaves window 0 off.
	static const char *const formats[8] = { "ARGB8888", "RGB888", "RGB565", "?", "YUV420", "YUV422", "YUV444", "?" };
	uint32_t w0 = rd(VOP + 0x50), w1 = rd(VOP + 0x90), info = rd(VOP + 0xa4);
	name("Window");
	kprintf("0 %s, 1 %s %s %ux%u\n", w0 & 1 ? "on" : "off", w1 & 1 ? "on" : "off",
		formats[w1 >> 4 & 7], (info & 0xfff) + 1, (info >> 16 & 0xfff) + 1);
	name("Reads");
	kprintf("%08x, %u bytes/line\n", rd(VOP + 0xa0), (rd(VOP + 0x98) & 0x1fff) * 4);
	uint32_t ctrl0 = rd(VOP + 0x20), ctrl2 = rd(VOP + 0x28);
	name("Out");
	kprintf("MIPI %s, RGB %s, %s%s\n", ctrl0 >> 24 & 1 ? "on" : "off", ctrl0 & 1 ? "on" : "off",
		ctrl2 >> 14 & 1 ? "BLANK " : "", rd(VOP + 0x18) >> 1 & 1 ? "standby" : "running");

	// The PHY's PLL makes the lane clock from 24 MHz: x fbdiv / prediv
	uint32_t prediv = phy(0, 3) & 0x1f, fbdiv = (phy(0, 3) >> 5 & 1) << 8 | phy(0, 4);
	uint32_t lane = prediv ? 24000 * fbdiv / prediv : 0;   // kHz = kbit/s
	uint32_t lanes = (rd(DSI + 0xa4) & 3) + 1;
	name("Lanes");
	kprintf("%u x %u Mbit/s = %u\n", lanes, lane / 1000, lanes * lane / 1000);
	name("Needs");
	kprintf("%u Mbit/s (24 bits a pixel)\n", c.dclk * 24 / 1000);
	uint32_t a0 = phy(0, 0), a1 = phy(0, 1);
	name("PHY");
	kprintf("PLL %s, lanes %c%c%c%c%c, x%u/%u\n", a1 & 1 ? "off" : "on",
		a0 & 0x40 ? 'C' : '-', a0 & 4 ? '0' : '-', a0 & 8 ? '1' : '-',
		a0 & 0x10 ? '2' : '-', a0 & 0x20 ? '3' : '-', fbdiv, prediv);

	static const char *const vmodes[4] = { "pulses", "events", "burst", "burst" };
	uint32_t vid = rd(DSI + 0x38), st = rd(DSI + 0xb0);
	name("DSI");
	kprintf("%s mode, %s, %s\n", rd(DSI + 0x34) & 1 ? "command" : "video",
		vmodes[vid & 3], rd(DSI + 0x04) & 1 ? "on" : "OFF");
	name("Status");
	kprintf("PHY %s, packets %u px\n", st & 1 ? "locked" : "NOT LOCKED", rd(DSI + 0x3c) & 0x3fff);

	uint32_t dr = rd(GPIO3 + 0x00), ddr = rd(GPIO3 + 0x04);
	name("Reset");
	kprintf("GPIO3 C0 %s %u: %s\n", ddr >> PANEL_RESET & 1 ? "out" : "in",
		dr >> PANEL_RESET & 1, dr >> PANEL_RESET & 1 ? "running" : "IN RESET");
	struct rail rails[RK817_RAILS];
	name("Power");
	if (rk817_rails(rails) == 0)
		kprintf("vcc_lcd %u.%03u V %s\n", rails[11].mv / 1000, rails[11].mv % 1000,
			rails[11].on ? "on" : "OFF");
	else
		kputs("(power chip didn't answer)\n");
}

// ---- the raw pages --------------------------------------------------------------
// A checksum over everything on the page (zlib's CRC-32 of each address and
// value, as little-endian 32-bit words) shows whether a copy of it is exact.

static uint32_t crc;

static void crc_word(uint32_t w)
{
	for (int i = 0; i < 4; i++) {
		crc ^= w >> (8 * i) & 0xff;
		for (int b = 0; b < 8; b++)
			crc = crc >> 1 ^ (0xedb88320U & -(crc & 1));
	}
}

static uint32_t shown, skipped;

// Is there room for one more? 19 lines of `per_line`.
static int room(uint32_t per_line)
{
	if (shown < 19 * per_line)
		return 1;
	skipped++;
	return 0;
}

// 3 a line: "offset value"
static void word(uint32_t offset, uint32_t value)
{
	if (!room(3))
		return;
	crc_word(offset);
	crc_word(value);
	console_color(GREY);
	kprintf("%03x ", offset);
	console_color(WHITE);
	kprintf("%08x", value);
	kputs(++shown % 3 ? " " : "\n");
}

static void vop_raw(void)
{
	kputs("VOP at ff460000, not zero:\n");
	for (uint32_t off = 0; off < 0x200; off += 4)
		if (rd(VOP + off))
			word(off, rd(VOP + off));
}

static void dsi_raw(void)
{
	kputs("DSI host at ff450000:\n");
	for (uint32_t off = 0; off <= 0xdc; off += 4)
		word(off, rd(DSI + off));
}

// Two pages: registers first to last-1, not zero. A register's number is
// its byte address / 4.
static void phy_raw(uint32_t first, uint32_t last)
{
	kprintf("PHY at ff2e0000, regs %02x-%02x:\n", first, last - 1);
	for (uint32_t r = first; r < last; r++) {
		uint32_t v = phy(r >> 5, r & 31);
		if (!v || !room(6))
			continue;
		crc_word(r);
		crc_word(v);
		console_color(GREY);
		kprintf("%02x", r);
		console_color(WHITE);
		kprintf("=%02x", v);
		kputs(++shown % 6 ? " " : "\n");
	}
}

static void misc_raw(void)
{
	static const uint32_t regs[] = {
		CRU + 0x40, CRU + 0x44, CRU + 0x48,           // CPLL
		CRU + 0x60, CRU + 0x64, CRU + 0x68,           // NPLL
		CRU + 0xa0,                                   // PLL modes
		CLKSEL(5), CLKSEL(6), CLKSEL(49),             // pixel clock, I2C clocks
		CLKGATE(2), CLKGATE(3), CLKGATE(16),          // display clock gates
		PMUCRU + 0x00, PMUCRU + 0x04, PMUCRU + 0x08,  // GPLL
		PMUCRU + 0x20, PMUCRU + 0x48, PMUCRU + 0x84,  // GPLL mode, PHY ref clock
		GRF + 0x438,                                  // DSI: which VOP, lane setup
		GPIO3 + 0x00, GPIO3 + 0x04,                   // panel reset pin
	};
	kputs("Clocks, GRF, GPIO3:\n");
	for (uint32_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		uint32_t v = rd(regs[i]);
		crc_word(regs[i]);
		crc_word(v);
		console_color(GREY);
		kprintf("%08x ", regs[i]);
		console_color(WHITE);
		kprintf("%08x", v);
		kputs(++shown % 2 ? "  " : "\n");
	}
}

void dump_page(uint32_t page)
{
	static const char *const titles[DUMP_PAGES] = {
		"The display, as U-Boot left it", "VOP registers", "DSI host registers",
		"Video PHY registers", "Video PHY registers", "Clock registers" };
	console_init(WHITE, BG);
	console_color(YELLOW);
	kprintf("%u/%u %s\n", page + 1, DUMP_PAGES, titles[page]);
	console_color(WHITE);
	crc = ~0U;
	shown = skipped = 0;
	switch (page) {
	case 0: summary(); break;
	case 1: vop_raw(); break;
	case 2: dsi_raw(); break;
	case 3: phy_raw(0x00, 0x80); break;   // analog, digital, clock lane, lane 0
	case 4: phy_raw(0x80, 0x100); break;  // lanes 1-3, LVDS
	case 5: misc_raw(); break;
	}
	if (page) {
		console_goto(0, 21);
		console_color(GREEN);
		kprintf("%u values, check %08x", shown, ~crc);
		if (skipped)
			kprintf(" +%u", skipped);
	}
}
