// The buttons. Every one is a switch from a GPIO pin to ground, with a pull-up
// resistor inside the chip: the pin reads 1 when released, 0 when pressed
// ("active low").
//
// The wiring comes from the console's device tree (rk3326-rg351mp-linux.dtb,
// "Game Console R35S/R36S fix by lxc"), nodes odroidgo3-keys and
// odroidgo3-joypad. The analog sticks aren't here: they go through the ADC.

#include "buttons.h"
#include "gpio.h"

const struct button buttons[] = {
	{ 1, 12, "U",    "DPAD-UP",      125, 175,  50, 50 },
	{ 1, 13, "D",    "DPAD-DOWN",    125, 275,  50, 50 },
	{ 1, 14, "L",    "DPAD-LEFT",     75, 225,  50, 50 },
	{ 1, 15, "R",    "DPAD-RIGHT",   175, 225,  50, 50 },
	{ 1,  7, "X",    "BTN-X",        465, 175,  50, 50 },
	{ 1,  5, "B",    "BTN-B",        465, 275,  50, 50 },
	{ 1,  6, "Y",    "BTN-Y",        415, 225,  50, 50 },
	{ 1,  2, "A",    "KEY BTN-A",    515, 225,  50, 50 },
	{ 3, 10, "L2",   "TOP-LEFT2",     40,  60, 110, 36 },
	{ 2,  6, "L1",   "TOP-LEFT",      40, 104, 110, 36 },
	{ 3, 15, "R2",   "TOP-RIGHT2",   490,  60, 110, 36 },
	{ 2,  7, "R1",   "TOP-RIGHT",    490, 104, 110, 36 },
	{ 2,  1, "VOL-", "BTN-VOLDN",    232,  60,  80, 36 },
	{ 2,  0, "VOL+", "BTN-VOLUP",    328,  60,  80, 36 },
	// the bottom row, left to right as on the console: stick clicks on the
	// ends, Select / FN / Start in the middle
	{ 2,  2, "L3",    "F3",          94, 345,  84, 40 },
	{ 3,  9, "SEL",   "F1",         186, 345,  84, 40 },
	{ 2,  4, "FN",    "F5",         278, 345,  84, 40 },
	{ 3, 12, "START", "F2",         370, 345,  84, 40 },
	{ 2,  3, "R3",    "F4",         462, 345,  84, 40 },
};

const uint32_t button_count = sizeof buttons / sizeof buttons[0];

void buttons_init(void)
{
	for (uint32_t i = 0; i < button_count; i++)
		gpio_input_pullup(buttons[i].bank, buttons[i].pin);
}

uint32_t buttons_pressed(void)
{
	uint32_t level[4] = { 0, gpio_read(1), gpio_read(2), gpio_read(3) };
	uint32_t pressed = 0;
	for (uint32_t i = 0; i < button_count; i++)
		if (!(level[buttons[i].bank] >> buttons[i].pin & 1))   // 0 = pressed
			pressed |= 1U << i;
	return pressed;
}
