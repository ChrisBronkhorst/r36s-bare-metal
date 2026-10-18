#pragma once
#include <stdint.h>

// Bring up the display with our own code: the panel, the screen controller
// (VOP), the DSI host and the video PHY, their clock, and the panel's power.
// running = 1: U-Boot's display is on; switch it off properly first.
// running = 0: nothing set up yet (we booted without U-Boot).
// The VOP shows screen.fb. Takes about a second, dark.
void display_start(int running);

int display_done(void);   // has display_start run?

// What happened, step by step, for the screen
void display_takeover_log(void);
