#pragma once
#include <stdint.h>

struct button {
	uint8_t bank, pin;     // where it's wired
	const char *name;      // shown on screen
	const char *dt_label;  // what the device tree calls it
	uint16_t x, y, w, h;   // where it's drawn
};

extern const struct button buttons[];
extern const uint32_t button_count;

void buttons_init(void);
uint32_t buttons_pressed(void);   // bit n set = buttons[n] is down

// Interrupts from the buttons' GPIO banks: from now on, any button change
// interrupts the CPU (and so wakes it from wfi).
void buttons_irq_init(void);
extern volatile uint64_t button_irqs;      // GPIO interrupts so far
extern volatile uint64_t button_changes;   // pin changes they reported
