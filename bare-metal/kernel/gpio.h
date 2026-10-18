#pragma once
#include <stdint.h>

// The RK3326 has 4 GPIO controllers ("banks" 0-3) of 32 pins each.

void gpio_input_pullup(uint32_t bank, uint32_t pin);   // for a button
void gpio_output(uint32_t bank, uint32_t pin, int on);  // for an LED
uint32_t gpio_read(uint32_t bank);   // all 32 pins at once, bit n = pin n
