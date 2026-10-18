#pragma once
#include <stdint.h>

// A report on the display pipeline exactly as U-Boot left it running: the
// clocks, the screen controller (VOP), the DSI host and the video PHY.
// Read-only. To take over the display ourselves, we have to reproduce this.

#define DUMP_PAGES 6
void dump_page(uint32_t page);   // 0 is the summary, then raw registers
uint32_t gpll_khz(void);         // the "general" PLL, which feeds many clocks
