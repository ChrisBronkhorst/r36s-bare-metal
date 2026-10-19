#pragma once
#include <stdint.h>
// Time a float matrix multiply four ways (plain, reordered, compiler NEON,
// hand NEON) for N x N, N a multiple of 8 up to 256
void matmul_bench(uint32_t n, void (*say)(const char *fmt, ...));
