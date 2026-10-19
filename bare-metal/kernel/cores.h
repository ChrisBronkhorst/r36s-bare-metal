#pragma once
// The other three CPU cores. They run jobs core 0 hands them, with
// interrupts off for good: nothing ever interrupts them.
#include <stdint.h>

// Start cores 1-3 (through the monitor, PSCI CPU_ON). 0, or what went wrong.
const char *cores_start(void);
int core_alive(uint32_t core);
// Give core `core` (1-3) a function to run; core_wait waits until it's done
void core_run(uint32_t core, void (*fn)(void *arg), void *arg);
void core_wait(uint32_t core);
int core_busy(uint32_t core);   // 1 while it has a job
uint64_t core_jobs(uint32_t core);
