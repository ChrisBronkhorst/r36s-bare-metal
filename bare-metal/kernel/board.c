// Board setup U-Boot used to do: clocks, pins, power rails.
//
// After reset, most clocks run but their dividers and sources are
// whatever the chip and Rockchip's loader left. So for I2C0 and the PWM
// we pick the simplest source there is: the 24 MHz crystal, undivided.
// Then the speed maths doesn't depend on any PLL.

#include "board.h"
#include "backlight.h"
#include "i2c.h"
#include "rk817.h"
#include "sysreg.h"

#define CRU    0xff2b0000UL
#define PMUGRF 0xff010000UL
#define CLKSEL(n)  (CRU + 0x100 + 4 * (n))
#define CLKGATE(n) (CRU + 0x200 + 4 * (n))

#define PWM1   0xff200010UL   // channel 1 of the PWM block at 0xff200000

struct board_log board_log;

static void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

// Rockchip "hiword mask" registers: top 16 bits = which bottom bits change
static void masked(uintptr_t a, uint32_t mask, uint32_t value)
{
	wr(a, mask << 16 | (value & mask));
}

static void i2c0_setup(void)
{
	// Pins GPIO0 B0 (SCL) and B1 (SDA): function 1 = I2C0. Bank 0's pin
	// functions live in the PMU's GRF, 2 bits a pin.
	masked(PMUGRF + 0x04, 0xf, 0x5);
	// clk_i2c0: CLKSEL49 bit 7 = source (1 = crystal), bits 0-6 = divider-1.
	// Gates: the clock itself (CLKGATE12 bit 0), the register side (14/10).
	masked(CLKSEL(49), 0xff, 1U << 7);
	masked(CLKGATE(12), 1U << 0, 0);
	masked(CLKGATE(14), 1U << 10, 0);
	// 100 kHz. One SCL period = 8 x (low + 1 + high + 1) clocks, and
	// 24 MHz / 100 kHz = 240 = 8 x 30: low = high = 14.
	wr(I2C0 + 0x04, 14U << 16 | 14);
	wr(I2C0 + 0x00, 0);   // controller off, no timing tweaks
}

static void pwm_setup(void)
{
	// Pin GPIO0 C0: function 1 = PWM1
	masked(PMUGRF + 0x08, 0x3, 0x1);
	// All four PWM channels share clk_pwm0: CLKSEL52 bit 7 = crystal.
	// Gates: CLKGATE12 bit 5 (clock), CLKGATE14 bit 15 (registers).
	masked(CLKSEL(52), 0xff, 1U << 7);
	masked(CLKGATE(12), 1U << 5, 0);
	masked(CLKGATE(14), 1U << 15, 0);
	// 40 kHz (the device tree's 25 us period): 600 ticks of 24 MHz.
	// Off for now. Control: enable, continuous, "on" = high.
	wr(PWM1 + 0x0c, 0);
	wr(PWM1 + 0x04, 600);
	wr(PWM1 + 0x08, 0);
	wr(PWM1 + 0x0c, 1U << 0 | 1U << 1 | 1U << 3);
}

int board_init(void)
{
	board_log.gpll_mode = rd(0xff2bc020);
	board_log.clksel3 = rd(CLKSEL(3));
	i2c0_setup();
	pwm_setup();
	backlight_init();   // reads the period we just set

	// The panel's power: vcc_backlight (LDO7, 3.3 V) and vcc_lcd (LDO8,
	// 2.8 V). Set their voltages first, then switch them on. vcc_lcd goes
	// on later, as part of the panel's start-up.
	board_log.i2c = i2c_read(I2C0, RK817_ADDR, 0xb1, board_log.rails_before, 4);
	if (board_log.i2c)
		return board_log.i2c;
	board_log.i2c = rk817_ldo_set_mv(7, 3300) | rk817_ldo_set_mv(8, 2800) |
		rk817_rail_enable(RAIL_VCC_BACKLIGHT, 1);
	return board_log.i2c;
}
