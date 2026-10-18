// Our own display bring-up, replacing U-Boot's.
//
//   framebuffer -> VOP -> DSI host -> video PHY -> 4 lanes -> panel
//
// The values come from two places: the registers U-Boot left (photographed
// from the display report, every page checked by CRC: display_tables.h), and
// the panel's start-up commands from the device tree U-Boot uses. The order
// follows Linux's drivers (dw-mipi-dsi.c, phy-rockchip-inno-dsidphy.c,
// rockchip_drm_vop.c) and Rockchip's panel driver.
//
// Going down:  panel off -> VOP standby -> panel reset + power off ->
//              everything held in reset -> clocks -> out of reset
// Coming up:   DSI host + PHY (command mode) -> panel power + reset ->
//              panel start-up commands -> video mode -> VOP -> backlight

#include "backlight.h"
#include "board.h"
#include "console.h"
#include "display.h"
#include "gpio.h"
#include "rk817.h"
#include "screen.h"
#include "sysreg.h"
#include "display_tables.h"

#define CRU     0xff2b0000UL
#define PHY     0xff2e0000UL
#define DSI     0xff450000UL
#define VOP     0xff460000UL

#define CLKSEL(n)  (CRU + 0x100 + 4 * (n))
#define CLKGATE(n) (CRU + 0x200 + 4 * (n))
#define SOFTRST(n) (CRU + 0x300 + 4 * (n))

// DSI host registers (Synopsys DesignWare)
#define DSI_PWR_UP     0x004
#define DSI_MODE_CFG   0x034   // 0 video, 1 command
#define DSI_VID_MODE   0x038
#define DSI_GEN_HDR    0x06c   // writing it sends a packet
#define DSI_CMD_STATUS 0x074
#define DSI_LPCLK_CTRL 0x094
#define DSI_PHY_RSTZ   0x0a0
#define DSI_PHY_STATUS 0x0b0
#define DSI_INT_ST0    0x0bc
#define DSI_INT_ST1    0x0c0
#define GEN_CMD_EMPTY  (1U << 0)
#define GEN_CMD_FULL   (1U << 1)
#define GEN_PLD_W_EMPTY (1U << 2)

#define PANEL_RESET_BANK 3   // GPIO3 C0, low = reset
#define PANEL_RESET_PIN  16

static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

// Rockchip clock/reset registers: the top 16 bits choose which bottom bits
// to change
static void masked(uintptr_t a, uint32_t mask, uint32_t value)
{
	wr(a, mask << 16 | (value & mask));
}

static uint64_t now(void) { return READ_SYSREG(cntpct_el0); }

static void delay_us(uint64_t us)
{
	uint64_t end = now() + us * 24;   // 24 counter ticks a microsecond
	while (now() < end)
		;
}

static void delay_ms(uint64_t ms) { delay_us(ms * 1000); }

// Wait until (register & mask) == want, at most `us` microseconds.
// Returns how long it took, or -1.
static int64_t wait_for(uintptr_t reg, uint32_t mask, uint32_t want, uint64_t us)
{
	uint64_t start = now();
	while ((rd(reg) & mask) != want)
		if (now() - start > us * 24)
			return -1;
	return (int64_t)((now() - start) / 24);
}

// A PHY register: number = byte address / 4
static void phy_wr(uint32_t reg, uint32_t v) { wr(PHY + (reg << 2), v); }
static uint32_t phy_rd(uint32_t reg) { return rd(PHY + (reg << 2)) & 0xff; }

// ---- what happened ------------------------------------------------------------

static struct {
	int bye;                 // goodbye commands sent to the panel
	int64_t cpll_us;         // PLL lock time
	int64_t phy_us;          // PHY PLL lock time
	int panel_power;         // I2C to the power chip worked (both ways)
	uint32_t sent, failed;   // panel start-up commands
	uint32_t vop_same, dsi_same, phy_same;   // read back = what we wrote
	uint32_t vop_reg, vop_got;               // the first VOP register that differs
	uint32_t errors0, errors1;               // DSI error flags at the end
	uint64_t dark_ms, total_ms;
	int done;
} log;

// ---- DSI commands ---------------------------------------------------------------

// Short packets carry a command and up to 2 bytes, all in the header:
// [data1][data0][virtual channel + packet type]. The host sends it in
// low-power mode (CMD_MODE_CFG says which kinds go LP) and we wait until
// it's gone.
static int dsi_short(uint32_t type, uint32_t cmd, uint32_t param)
{
	if (wait_for(DSI + DSI_CMD_STATUS, GEN_CMD_FULL, 0, 20000) < 0)
		return -1;
	wr(DSI + DSI_GEN_HDR, param << 16 | cmd << 8 | type);
	uint32_t empty = GEN_CMD_EMPTY | GEN_PLD_W_EMPTY;
	return wait_for(DSI + DSI_CMD_STATUS, empty, empty, 20000) < 0 ? -1 : 0;
}

// Command mode: we send packets ourselves. Video mode: the host streams
// the VOP's pixels. Switch with the host powered down, as Linux does.
static void dsi_mode(int video)
{
	wr(DSI + DSI_PWR_UP, 0);
	wr(DSI + DSI_MODE_CFG, video ? 0 : 1);
	if (video)
		wr(DSI + DSI_VID_MODE, 0xbf02);   // burst, low power in the gaps
	wr(DSI + DSI_LPCLK_CTRL, 1);          // the clock lane runs at high speed
	wr(DSI + DSI_PWR_UP, 1);
}

// ---- going down -----------------------------------------------------------------

static void shut_down(int running)
{
	if (running) {
		// Tell the panel: display off (0x28), then sleep (0x10)
		dsi_mode(0);
		log.bye = dsi_short(0x05, 0x28, 0) == 0;
		delay_ms(20);
		log.bye &= dsi_short(0x05, 0x10, 0) == 0;
		delay_ms(120);   // the panel needs a moment to enter sleep

		// VOP: standby (stops at the end of the current frame), wait a frame
		wr(VOP + 0x018, rd(VOP + 0x018) | 1U << 1);
		wr(VOP + 0x000, 1);          // REG_CFG_DONE: apply
		delay_ms(30);
	} else {
		log.bye = -1;   // nothing running, nobody to say goodbye to
	}

	// The panel: into reset, power off
	gpio_output(PANEL_RESET_BANK, PANEL_RESET_PIN, 0);
	log.panel_power = rk817_rail_enable(RAIL_VCC_LCD, 0) == 0;

	// DSI host and PHY off
	wr(DSI + DSI_PWR_UP, 0);
	wr(DSI + DSI_PHY_RSTZ, 0);

	// Hold the VOP (registers + pixel side), DSI host and PHY in reset.
	// Reset numbers from px30-cru.h: VOPB_H 52, VOPB 53, MIPIDSI_HOST_P 61,
	// MIPIDSIPHY_P 62; 16 per register. Not the VOP's memory side
	// (VOPB_A): resetting that mid-transfer can hang the bus.
	masked(SOFTRST(3), 1U << 4 | 1U << 5 | 1U << 13 | 1U << 14, ~0U);
}

// ---- clocks -------------------------------------------------------------------------

static void clocks(void)
{
	// CPLL: 24 MHz / 3 x 145 / 2 / 1 = 580 MHz. Switch it to "slow" (the
	// crystal, straight through) while changing it, wait for lock, switch back.
	masked(CRU + 0xa0, 3U << 2, 0);                    // slow
	masked(CRU + 0x40, 0x7fff, 2U << 12 | 145);        // postdiv1, fbdiv
	masked(CRU + 0x44, 0x31ff, 1U << 12 | 1U << 6 | 3); // powered, integer, postdiv2, refdiv
	log.cpll_us = wait_for(CRU + 0x44, 1U << 10, 1U << 10, 10000);
	masked(CRU + 0xa0, 3U << 2, 1U << 2);              // normal

	// Pixel clock dclk_vopb: CPLL / 20 = 29 MHz, not through the
	// fractional divider
	masked(CLKSEL(5), 3U << 14 | 1U << 11 | 0xff, 20 - 1);
}

// Every clock gate for the display open, as U-Boot left them: the VOP's
// bus and pixel clocks (CLKGATE2 and 3), the PHY's register clock (16).
// First thing: touching a block whose register clock is gated can hang
// the bus.
static void open_gates(void)
{
	wr(CLKGATE(2), 0xffff0000);
	wr(CLKGATE(3), 0xffff0000);
	wr(CLKGATE(16), 0xffff0000);
	// and the big VOP (not the little one) feeds the DSI host
	wr(0xff140438, 0xffff0000);   // GRF_PD_VO_CON1
}

// ---- coming up ------------------------------------------------------------------------

static void vop_up(void)
{
	for (uint32_t i = 0; i < sizeof vop_regs / sizeof vop_regs[0]; i++)
		wr(VOP + vop_regs[i][0], vop_regs[i][1]);
	wr(VOP + 0x0a0, (uint32_t)(uintptr_t)screen.fb);   // window 1 reads our framebuffer
	wr(VOP + 0x000, 1);   // REG_CFG_DONE: all of it takes effect at once
}

static void dsi_phy_up(void)
{
	wr(DSI + DSI_PWR_UP, 0);
	for (uint32_t i = 0; i < sizeof dsi_regs / sizeof dsi_regs[0]; i++)
		wr(DSI + dsi_regs[i][0], dsi_regs[i][1]);

	// The PHY, in Linux's order. Analog register 0: bandgap (the voltage
	// reference) on, "power work" on. LVDS part, register 3: MIPI mode.
	phy_wr(0x00, (phy_rd(0x00) & ~0x83U) | 0x01);
	phy_wr(0xe3, (phy_rd(0xe3) & ~7U) | 0x01);
	// Its PLL: 24 MHz x 16 / 1 = 384 MHz, the bit rate of each lane
	phy_wr(0x03, 0x01);
	phy_wr(0x04, 0x10);
	phy_wr(0x01, phy_rd(0x01) & ~3U);     // PLL and LDO on
	phy_wr(0x01, phy_rd(0x01) | 4U);      // analog reset pulse
	delay_us(1);
	phy_wr(0x01, phy_rd(0x01) & ~4U);
	phy_wr(0x20, phy_rd(0x20) & ~1U);     // digital reset pulse
	delay_us(1);
	phy_wr(0x20, phy_rd(0x20) | 1U);
	// Everything else as U-Boot had it: each lane's timings (how long to
	// hold each state when switching between low power and high speed)
	for (uint32_t i = 0; i < sizeof phy_regs / sizeof phy_regs[0]; i++) {
		uint32_t r = phy_regs[i][0];
		if (r != 0x00 && r != 0x01 && r != 0x03 && r != 0x04 && r != 0x20 && r != 0xe3)
			phy_wr(r, phy_regs[i][1]);
	}
	phy_wr(0x00, 0x7d);   // and finally: clock lane + 4 data lanes on

	// The host lets the PHY run: enable clock, out of shutdown, out of reset
	wr(DSI + DSI_PHY_RSTZ, 7);
	log.phy_us = wait_for(DSI + DSI_PHY_STATUS, 1, 1, 10000);
	rd(DSI + DSI_INT_ST0);   // reading clears old error flags
	rd(DSI + DSI_INT_ST1);
	dsi_mode(0);
}

static void panel_up(void)
{
	// Power, then a clean reset pulse, as the panel driver does
	log.panel_power &= rk817_rail_enable(RAIL_VCC_LCD, 1) == 0;
	delay_ms(20);
	gpio_output(PANEL_RESET_BANK, PANEL_RESET_PIN, 0);
	delay_ms(150);
	gpio_output(PANEL_RESET_BANK, PANEL_RESET_PIN, 1);
	delay_ms(50);

	// The start-up commands: mostly the panel maker's register settings
	// (0xff 0x30 0x52 0x0n selects a page of them), then sleep out (0x11)
	// and display on (0x29)
	for (uint32_t i = 0; i < sizeof panel_init / sizeof panel_init[0]; i++) {
		const uint8_t *c = panel_init[i];
		if (dsi_short(c[0], c[2], c[3]) == 0)
			log.sent++;
		else
			log.failed++;
		if (c[1])
			delay_ms(c[1]);
	}
}

// Read back what we wrote: does the hardware now hold U-Boot's values?
static void check(void)
{
	for (uint32_t i = 0; i < sizeof vop_regs / sizeof vop_regs[0]; i++) {
		uint32_t got = rd(VOP + vop_regs[i][0]);
		// 0x1f0 changes by itself: U-Boot's dump had 0x179, ours read
		// 0x092, both below the 512 lines of a frame. It looks like the
		// line being sent right now, so it can't be compared.
		if (got == vop_regs[i][1] || vop_regs[i][0] == 0x1f0)
			log.vop_same++;
		else if (!log.vop_got && !log.vop_reg) {
			log.vop_reg = vop_regs[i][0];
			log.vop_got = got;
		}
	}
	for (uint32_t i = 0; i < sizeof dsi_regs / sizeof dsi_regs[0]; i++)
		log.dsi_same += rd(DSI + dsi_regs[i][0]) == dsi_regs[i][1];
	for (uint32_t i = 0; i < sizeof phy_regs / sizeof phy_regs[0]; i++)
		log.phy_same += phy_rd(phy_regs[i][0]) == phy_regs[i][1];
	log.errors0 = rd(DSI + DSI_INT_ST0);
	log.errors1 = rd(DSI + DSI_INT_ST1);
}

// Without U-Boot there's no picture until the end, so the backlight
// reports progress: one flash after each step. N flashes and then dark
// = stuck in step N + 1. (Flash 1 is the power chip, in main.c.)
static int flashing;

static void step_done(void)
{
	if (!flashing)
		return;
	backlight_set(100);
	delay_ms(150);
	backlight_set(0);
	delay_ms(450);
}

void display_start(int running)
{
	uint64_t start = now();
	uint32_t light = backlight_get();
	flashing = !running;
	backlight_set(0);
	open_gates();
	shut_down(running);
	step_done();     // 2: clock gates open, panel off
	clocks();
	step_done();     // 3: CPLL and the pixel clock set
	delay_ms(1);
	masked(SOFTRST(3), 1U << 4 | 1U << 5 | 1U << 13 | 1U << 14, 0);   // out of reset
	delay_ms(200);   // let the panel's power drain away completely
	step_done();     // 4: out of reset
	dsi_phy_up();
	step_done();     // 5: DSI host and PHY up
	panel_up();
	step_done();     // 6: panel powered, commands sent
	dsi_mode(1);     // video: the host now takes pixels from the VOP...
	step_done();     // 7: video mode
	vop_up();        // ...so only now start sending them. Started earlier,
	                 // they overflow the host's input (DSI error bit 0x80).
	step_done();     // 8: VOP running
	delay_ms(120);
	backlight_set(light ? light : 50);
	log.dark_ms = (now() - start) / 24000;
	log.total_ms = (now() - start) / 24000;
	check();
	log.done = 1;
}

static void row(const char *name, int ok)
{
	console_color(GREY);
	uint32_t n = 0;
	for (; name[n]; n++)
		kputc(name[n]);
	for (; n < 10; n++)
		kputc(' ');
	console_color(ok ? GREEN : YELLOW);
}

int display_done(void)
{
	return log.done;
}

void display_takeover_log(void)
{
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("Our own display!\n\n");
	if (!log.done) {
		kputs("(not taken over yet)\n");
		return;
	}
	row("Goodbye", log.bye != 0);
	kputs(log.bye < 0 ? "nothing was running\n" : log.bye ? "display off, sleep: sent\n" : "panel didn't take it\n");
	row("Clock", log.cpll_us >= 0);
	if (log.cpll_us >= 0)
		kprintf("CPLL 580 MHz, lock %ld us\n", log.cpll_us);
	else
		kputs("CPLL never locked\n");
	row("PHY", log.phy_us >= 0);
	if (log.phy_us >= 0)
		kprintf("384 MHz, lock %ld us\n", log.phy_us);
	else
		kputs("PLL never locked\n");
	row("Power", log.panel_power);
	kputs(log.panel_power ? "vcc_lcd off, then on\n" : "power chip didn't answer\n");
	row("Commands", log.failed == 0);
	kprintf("%u sent, %u failed\n", log.sent, log.failed);
	uint32_t nv = sizeof vop_regs / sizeof vop_regs[0];
	uint32_t nd = sizeof dsi_regs / sizeof dsi_regs[0];
	uint32_t np = sizeof phy_regs / sizeof phy_regs[0];
	row("VOP", log.vop_same == nv);
	if (log.vop_same == nv)
		kprintf("%u of %u match U-Boot\n", log.vop_same, nv);
	else
		kprintf("%u/%u, %03x is %08x\n", log.vop_same, nv, log.vop_reg, log.vop_got);
	row("DSI", log.dsi_same == nd);
	kprintf("%u of %u match U-Boot\n", log.dsi_same, nd);
	row("PHY", log.phy_same == np);
	kprintf("%u of %u match U-Boot\n", log.phy_same, np);
	row("Errors", !log.errors0 && !log.errors1);
	kprintf("DSI %x %x\n", log.errors0, log.errors1);
	if (log.bye < 0) {   // started without U-Boot: what the loader left
		row("Loader", 1);
		kprintf("GPLL mode %x, bus %x\n", board_log.gpll_mode, board_log.clksel3);
		row("Rails", 1);
		kprintf("were %02x %02x %02x %02x\n", board_log.rails_before[0],
			board_log.rails_before[1], board_log.rails_before[2], board_log.rails_before[3]);
	}
	row("Dark", 1);
	kprintf("%lu ms\n", log.dark_ms);
}
