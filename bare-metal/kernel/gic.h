#pragma once
#include <stdint.h>

// The GIC-400 interrupt controller: every device's interrupt wire comes in
// here, and it decides which ones reach the CPU.
//   0-15  SGIs: cores poking each other (no wire)
//   16-31 PPIs: private to each core, like its timer
//   32+   SPIs: shared device wires (device tree number + 32)

#define GIC_SPURIOUS 1023   // "nothing for you after all"

void gic_init(void);
void gic_enable(uint32_t id, uint8_t priority);   // lower number = more urgent
uint32_t gic_ack(void);       // which interrupt is it? (and mark it active)
void gic_done(uint32_t id);   // finished with it ("end of interrupt")
int gic_pending(uint32_t id); // is it waiting? (for debugging)
