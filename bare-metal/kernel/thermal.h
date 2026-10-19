#pragma once
// The chip's temperature sensor (TSADC): two channels, the CPU and the GPU
#include <stdint.h>

void thermal_init(void);                     // start it measuring; arm the 95 C reset
int thermal_read(uint32_t channel, int32_t *celsius_tenths);   // 0 if there's a reading
