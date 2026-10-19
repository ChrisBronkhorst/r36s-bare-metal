// Matrix multiply, four ways: C = A x B, N x N floats, row by row in memory.
//
// The only file in the kernel allowed to use the floating point / SIMD
// registers (the Makefile leaves out -mgeneral-regs-only for it). That's
// safe without saving them anywhere: it only runs from the shell, with
// interrupts off, and no other kernel code ever touches those registers.
//
// NEON: 32 registers of 128 bits, v0-v31. One instruction works on all of
// a register: here, 4 floats at once. vfmaq_n_f32(acc, b, a) is
// acc[0..3] += b[0..3] * a: four multiply-adds in one instruction ("fused
// multiply-add": multiply and add with a single rounding at the end).

#include <arm_neon.h>
#include <stdint.h>
#include "matmul.h"
#include "sysreg.h"

#define MAX_N 256
static float A[MAX_N * MAX_N] __attribute__((aligned(64)));
static float B[MAX_N * MAX_N] __attribute__((aligned(64)));
static float C[MAX_N * MAX_N] __attribute__((aligned(64)));
static float ref[MAX_N * MAX_N] __attribute__((aligned(64)));

static void zero(float *m, uint32_t n)
{
#pragma clang loop vectorize(disable) interleave(disable)
	for (uint32_t i = 0; i < n * n; i++)
		m[i] = 0;
}

// 1. The textbook order. The inner loop walks down a column of B: each
// step jumps N floats ahead in memory, so almost every read is a new cache
// line, and only 4 of its 64 bytes get used.
static void plain_ijk(uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		for (uint32_t j = 0; j < n; j++) {
			float sum = 0;
#pragma clang loop vectorize(disable) interleave(disable)
			for (uint32_t k = 0; k < n; k++)
				sum += A[i * n + k] * B[k * n + j];
			C[i * n + j] = sum;
		}
}

// 2. Same sums, loops swapped: take one A[i][k], and add A[i][k] x (row k
// of B) to row i of C. Now B and C are read along their rows: every byte
// of every cache line gets used.
static void plain_ikj(uint32_t n)
{
	zero(C, n);
	for (uint32_t i = 0; i < n; i++)
		for (uint32_t k = 0; k < n; k++) {
			float a = A[i * n + k];
#pragma clang loop vectorize(disable) interleave(disable)
			for (uint32_t j = 0; j < n; j++)
				C[i * n + j] += a * B[k * n + j];
		}
}

// 3. Exactly the same code, but the compiler may vectorise it: the inner
// loop is "row of C += a x row of B", which turns into NEON easily
static void auto_ikj(uint32_t n)
{
	zero(C, n);
	for (uint32_t i = 0; i < n; i++)
		for (uint32_t k = 0; k < n; k++) {
			float a = A[i * n + k];
			for (uint32_t j = 0; j < n; j++)
				C[i * n + j] += a * B[k * n + j];
		}
}

// 4. By hand: a 4 x 8 block of C lives in 8 registers for the whole k
// loop and is written to memory once at the end. For each k: 2 loads of B
// (8 floats), 4 of A (one float each), 8 vector multiply-adds = 32
// multiply-adds. Version 3 must load and store C for every 4 multiply-adds.
static void neon_4x8(uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4)
		for (uint32_t j = 0; j < n; j += 8) {
			float32x4_t c00 = vdupq_n_f32(0), c01 = c00, c10 = c00, c11 = c00;
			float32x4_t c20 = c00, c21 = c00, c30 = c00, c31 = c00;
			const float *a = &A[i * n], *b = &B[j];
			for (uint32_t k = 0; k < n; k++) {
				float32x4_t b0 = vld1q_f32(b + k * n), b1 = vld1q_f32(b + k * n + 4);
				float a0 = a[k], a1 = a[n + k], a2 = a[2 * n + k], a3 = a[3 * n + k];
				c00 = vfmaq_n_f32(c00, b0, a0), c01 = vfmaq_n_f32(c01, b1, a0);
				c10 = vfmaq_n_f32(c10, b0, a1), c11 = vfmaq_n_f32(c11, b1, a1);
				c20 = vfmaq_n_f32(c20, b0, a2), c21 = vfmaq_n_f32(c21, b1, a2);
				c30 = vfmaq_n_f32(c30, b0, a3), c31 = vfmaq_n_f32(c31, b1, a3);
			}
			float *c = &C[i * n + j];
			vst1q_f32(c, c00), vst1q_f32(c + 4, c01);
			vst1q_f32(c + n, c10), vst1q_f32(c + n + 4, c11);
			vst1q_f32(c + 2 * n, c20), vst1q_f32(c + 2 * n + 4, c21);
			vst1q_f32(c + 3 * n, c30), vst1q_f32(c + 3 * n + 4, c31);
		}
}

static uint64_t ticks(void) { return READ_SYSREG(cntpct_el0); }

#define RUNS 50

static void sort(uint64_t *t, uint32_t n)   // insertion sort: n is small
{
	for (uint32_t i = 1; i < n; i++)
		for (uint32_t j = i; j > 0 && t[j - 1] > t[j]; j--) {
			uint64_t x = t[j];
			t[j] = t[j - 1], t[j - 1] = x;
		}
}

// A time in ticks (24 per microsecond) as microseconds with one decimal
static void say_us(void (*say)(const char *fmt, ...), uint64_t t)
{
	uint64_t tenths = t * 10 / 24;
	say("%8lu.%lu", tenths / 10, tenths % 10);
}

// The biggest difference from the reference, relative to the biggest
// value, in parts per million. Different orders of adding round
// differently, so a little is expected; a mistake shows as ~1000000.
static uint32_t error_ppm(uint32_t n)
{
	float worst = 0, biggest = 0;
	for (uint32_t i = 0; i < n * n; i++) {
		float d = C[i] - ref[i], r = ref[i];
		if (d < 0) d = -d;
		if (r < 0) r = -r;
		if (d > worst) worst = d;
		if (r > biggest) biggest = r;
	}
	return biggest > 0 ? (uint32_t)(worst / biggest * 1e6f) : 0;
}

void matmul_bench(uint32_t n, void (*say)(const char *fmt, ...))
{
	if (n < 8 || n > MAX_N || n % 8) {
		say("N: a multiple of 8, 8 to %u\n", MAX_N);
		return;
	}
	// Numbers between -1 and 1, the same every time
	uint64_t h = 88172645463325252ULL;
	for (uint32_t i = 0; i < n * n; i++) {
		h ^= h << 13, h ^= h >> 7, h ^= h << 17;
		A[i] = (float)((int32_t)(h % 2001) - 1000) / 1000;
		h ^= h << 13, h ^= h >> 7, h ^= h << 17;
		B[i] = (float)((int32_t)(h % 2001) - 1000) / 1000;
	}

	static const struct {
		const char *name;
		void (*run)(uint32_t n);
	} versions[] = {
		{ "plain i-j-k  ", plain_ijk },
		{ "plain i-k-j  ", plain_ikj },
		{ "compiler NEON", auto_ikj },
		{ "hand NEON 4x8", neon_4x8 },
	};
	uint64_t mads = (uint64_t)n * n * n;   // multiply-adds
	say("%ux%u: %lu multiply-adds, %u KB of data\n", n, n, mads, 3 * n * n * 4 / 1024);
	say("              ms     GFLOPS  flops/cycle  speed-up  error\n");
	uint64_t first = 0;
	static uint64_t times[4][RUNS];
	static uint32_t runs[4];
	for (uint32_t v = 0; v < sizeof versions / sizeof versions[0]; v++) {
		// Run it again and again (at least 5 times, up to 50 or about
		// 0.3 s), keeping every time. The first run also pays for filling
		// the caches: that's warm-up, not noise, so it isn't kept.
		uint64_t spent = 0;
		versions[v].run(n);
		runs[v] = 0;
		while (runs[v] < 5 || (spent < 24000 * 300 && runs[v] < RUNS)) {
			uint64_t t = ticks();
			versions[v].run(n);
			t = ticks() - t;
			spent += t;
			times[v][runs[v]++] = t;
		}
		sort(times[v], runs[v]);
		uint64_t best = times[v][0];
		if (v == 0) {
			for (uint32_t i = 0; i < n * n; i++)
				ref[i] = C[i];
			first = best;
		}
		// flops = 2 per multiply-add; ticks are 1/24 us
		uint64_t mflops = mads * 2 * 24 / (best ? best : 1);   // per second, millions
		uint64_t mhz = 1296;
		uint64_t fpc100 = mflops * 100 / mhz;   // flops per cycle x 100
		uint64_t up10 = first * 10 / (best ? best : 1);
		say("%s %4lu.%02lu  %3lu.%02lu    %lu.%02lu         %3lu.%lux    %u ppm\n",
		    versions[v].name, best / 24000, best / 240 % 100,
		    mflops / 1000, mflops / 10 % 100, fpc100 / 100, fpc100 % 100,
		    up10 / 10, up10 % 10, v ? error_ppm(n) : 0);
	}

	// How much the same work varies from run to run, in microseconds.
	// The spread is (slowest - fastest) / fastest.
	say("\nruns        fastest us   median us  slowest us  spread\n");
	for (uint32_t v = 0; v < sizeof versions / sizeof versions[0]; v++) {
		uint64_t *t = times[v], lo = t[0], hi = t[runs[v] - 1];
		uint64_t ppm = (hi - lo) * 1000000 / (lo ? lo : 1);
		say("%s %2u", versions[v].name, runs[v]);
		say_us(say, lo), say_us(say, t[runs[v] / 2]), say_us(say, hi);
		say("  %lu.%04lu%%\n", ppm / 10000, ppm % 10000);
	}
}
