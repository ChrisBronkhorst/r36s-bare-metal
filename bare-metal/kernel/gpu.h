#pragma once
// The Mali-G31 GPU, driven directly: power, reset, its MMU, and jobs.
#include <stdint.h>

// Power it on and set it up (once); prints what it finds. 0 if it worked.
int gpu_start(void (*say)(const char *fmt, ...));
// Run a first job chain: the GPU writes a number and its clock to RAM
void gpu_job_test(void (*say)(const char *fmt, ...));
// Run a first shader: 64 threads, each writes out[id] = id*id + 7
void gpu_compute_test(void (*say)(const char *fmt, ...));
// Matrix multiply on the GPU (which: 0 = one element per thread, 1 = a 4x4
// block per thread); n a multiple of 16. 0, or -1.
int gpu_matmul(int which, const float *a, const float *b, float *c, uint32_t n);
// Start the GPU if it isn't yet (quietly if say prints nothing). 1 if ready.
int gpu_ready(void (*say)(const char *fmt, ...));
// Check a matrix multiply shader: identity x B must give B exactly
void gpu_matmul_test(int which, uint32_t n, void (*say)(const char *fmt, ...));
