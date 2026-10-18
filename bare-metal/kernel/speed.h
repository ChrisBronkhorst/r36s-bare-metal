#pragma once
#include <stdint.h>

// Three small benchmarks, timed in counter ticks (24 million a second).
struct speed {
	uint64_t code;     // a CRC over 4 KB: mostly instructions
	uint64_t ram;      // write and read back 64 KB of RAM
	uint64_t screen;   // fill the whole screen
};

void speed_test(struct speed *s, uint32_t screen_rgb);
