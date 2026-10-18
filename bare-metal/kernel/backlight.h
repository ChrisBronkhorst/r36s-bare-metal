#pragma once
#include <stdint.h>

// The screen's backlight: LEDs behind the LCD, driven by PWM channel 1.
// PWM ("pulse-width modulation") switches them fully on and off about 16,000
// times a second; the share of each cycle they're on sets the brightness.

void backlight_init(void);             // read what U-Boot set up
uint32_t backlight_get(void);          // brightness, 0-100 %
void backlight_set(uint32_t percent);  // 0 = off
uint32_t backlight_period(void);       // one cycle, in PWM clock ticks
