#pragma once
// Receive a new kernel over the network and start it (`make push` on the Mac)
#include <stdint.h>

#define LOADER_PORT 5001

// One UDP packet for the loader. Fills `reply` (up to 16 bytes) and returns
// its length; *go is set when, after the reply has gone out, we should
// start the new kernel.
uint32_t loader_packet(const uint8_t *data, uint32_t len, uint8_t *reply, int *go);
void loader_start_new(void);   // never returns
void loader_lines(uint32_t row);
