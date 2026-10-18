#pragma once
#include <stdint.h>

// The RK3326 has 4 GPIO controllers ("banks" 0-3) of 32 pins each.

void gpio_input_pullup(uint32_t bank, uint32_t pin);   // for a button
void gpio_output(uint32_t bank, uint32_t pin, int on);  // for an LED
uint32_t gpio_read(uint32_t bank);   // all 32 pins at once, bit n = pin n

// Interrupts: watch a pin, and the bank raises its interrupt whenever it
// changes. gpio_changed() (in the handler) says which pins changed, and
// re-arms them for the next change.
#define GPIO_IRQ(bank) (35 + (bank))   // GIC numbers: device tree 3-6, + 32
void gpio_watch(uint32_t bank, uint32_t pin);
uint32_t gpio_changed(uint32_t bank);
