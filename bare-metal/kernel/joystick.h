#pragma once
#include <stdint.h>
// The analogue sticks (joystick.c)
int32_t adc_read(uint32_t channel);            // 0-1023, or -1
int32_t joy_raw(uint32_t position, int en);    // through the board's 4-way switch
#define JOY_RY 0
#define JOY_RX 1
#define JOY_LY 2
#define JOY_LX 3
void joy_rest(void);                // note where the sticks rest (leave them alone)
int32_t joy_axis(uint32_t axis);    // -512..511 from rest, 0 in the dead zone
