#pragma once
#include <stdint.h>

// What U-Boot used to set up for us, for when we boot without it:
// the clocks and pins of I2C0 (to the power chip) and of the backlight's
// PWM, and the panel's two power rails. Returns 0 if the power chip
// answered.
int board_init(void);

// What it found, for the info page
struct board_log {
	uint32_t gpll_mode, clksel3;   // the display's bus clock, as the loader left it
	uint8_t rails_before[4];       // the power chip's on/off registers
	int i2c;                       // 0 = power chip answered
};
extern struct board_log board_log;
