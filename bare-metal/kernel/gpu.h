#pragma once
// The Mali-G31 GPU, driven directly: power, reset, its MMU, and jobs.
#include <stdint.h>

// Power it on and set it up (once); prints what it finds. 0 if it worked.
int gpu_start(void (*say)(const char *fmt, ...));
// Run a first job chain: the GPU writes a number and its clock to RAM
void gpu_job_test(void (*say)(const char *fmt, ...));
// Run a first shader: 64 threads, each writes out[id] = id*id + 7
void gpu_compute_test(void (*say)(const char *fmt, ...));
