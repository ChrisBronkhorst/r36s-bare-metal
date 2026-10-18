#pragma once
#include <stdint.h>

// Take over the display from U-Boot: switch the panel off, reset the screen
// controller (VOP), the DSI host and the video PHY, set up their clock, power
// the panel off and on, and bring everything back up with our own code.
// The framebuffer stays where it is. Takes about a second, dark.
void display_takeover(void);

// What happened, step by step, for the screen
void display_takeover_log(void);
