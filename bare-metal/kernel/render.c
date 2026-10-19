// Software 3D: a spinning triangle, drawn by the CPU, pixel by pixel.
//
// The steps, every frame:
//   1. Move the triangle's three corners in 3D (turn it about two axes,
//      push it away from the eye).
//   2. Project them onto the screen: x/z and y/z, so further = smaller.
//   3. Rasterise: for every pixel in the triangle's bounding box, decide
//      "inside or outside" with three edge functions, and colour it by
//      blending the corners' colours.
//
// Double buffering: we draw into one of two pictures of our own while the
// display controller (VOP) shows the other, then tell the VOP to switch.
// It switches at the start of the next frame it sends to the LCD, so you
// never see a half-drawn picture. The console's own picture (screen.fb) is
// left alone and comes back when we stop.
//
// It all runs on core 3, which nothing interrupts, so core 0 stays free for
// the menu and the network. Like matmul.c, this file may use floating point
// (the Makefile leaves out -mgeneral-regs-only for it); no other code on
// core 3 does, so nothing else needs those registers saved.

#include <arm_neon.h>
#include <stdint.h>
#include "buttons.h"
#include "cores.h"
#include "render.h"
#include "screen.h"
#include "sysreg.h"

#define CORE 3

// The VOP's registers (Linux's rockchip_vop_reg.h, PX30)
#define REG_CFG_DONE 0x00   // write 1: new settings take effect at the next frame
#define INTR_CLEAR   0x38   // write-1-to-clear, bits 16-31 say which bits to write
#define INTR_STATUS  0x3c   // bit 16: a frame has started (raw, even if not enabled)
#define WIN0_MST     0x60   // where window 0's picture is
#define WIN1_MST     0xa0   // where window 1's picture is

#define MAX_BYTES (640 * 480 * 4)
static uint8_t frame[2][MAX_BYTES] __attribute__((aligned(64)));
static uint8_t background[MAX_BYTES] __attribute__((aligned(64)));

static volatile int stop;
static volatile int running;
static struct render_stats stats;

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint64_t ticks(void) { return READ_SYSREG(cntpct_el0); }

// ---- a little maths: there's no maths library -------------------------------

#define PI 3.14159265f

// sin x: bring x into -pi..pi, then the Taylor series to x^13 (error < 2e-5)
static float sine(float x)
{
	while (x > PI)
		x -= 2 * PI;
	while (x < -PI)
		x += 2 * PI;
	float x2 = x * x, term = x, sum = x;
	for (int n = 2; n <= 12; n += 2) {
		term *= -x2 / (float)(n * (n + 1));
		sum += term;
	}
	return sum;
}
static float cosine(float x) { return sine(x + PI / 2); }

// ---- pixels -----------------------------------------------------------------

// One pixel in the screen's own format (as in screen.c, but into ordinary,
// cached memory, so plain stores)
static inline void put(uint8_t *line, uint32_t x, uint32_t r, uint32_t g, uint32_t b)
{
	switch (screen.format) {
	case FMT_ARGB8888:
		((uint32_t *)line)[x] = 0xff000000 | r << 16 | g << 8 | b;
		break;
	case FMT_RGB888:
		line += x * 3;
		line[0] = (uint8_t)b, line[1] = (uint8_t)g, line[2] = (uint8_t)r;
		break;
	default:   // RGB565
		((uint16_t *)line)[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		break;
	}
}

// Dark blue at the top fading to black, with a faint grid: something to
// see the triangle against
static void make_background(void)
{
	for (uint32_t y = 0; y < screen.height; y++) {
		uint8_t *line = background + y * screen.pitch;
		uint32_t fade = 60 * (screen.height - y) / screen.height;
		for (uint32_t x = 0; x < screen.width; x++) {
			int grid = x % 40 == 0 || y % 40 == 0;
			put(line, x, fade / 3 + grid * 20, fade / 2 + grid * 20, fade + grid * 30);
		}
	}
}

// 16 bytes at a time, with NEON (a plain loop the compiler might turn into a
// call to our memcpy, which goes a byte at a time)
static void copy(uint8_t *dst, const uint8_t *src, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes; i += 64) {
		uint8x16_t a = vld1q_u8(src + i), b = vld1q_u8(src + i + 16);
		uint8x16_t c = vld1q_u8(src + i + 32), d = vld1q_u8(src + i + 48);
		vst1q_u8(dst + i, a), vst1q_u8(dst + i + 16, b);
		vst1q_u8(dst + i + 32, c), vst1q_u8(dst + i + 48, d);
	}
}

// Write a picture out of our cache to RAM, where the VOP reads it
static void clean(const uint8_t *p, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes; i += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(p + i) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

// ---- the triangle -----------------------------------------------------------

struct vertex {
	float x, y, z;    // on the screen: pixels, and the distance from the eye
	float r, g, b;
};

// Rasterise. Corners in screen pixels; colours blended "perspective
// correct": what's linear across the screen is colour/z and 1/z, not the
// colour itself (which is linear across the triangle in 3D).
static void triangle(uint8_t *pic, struct vertex v0, struct vertex v1, struct vertex v2,
		     float shade)
{
	// Fixed point, 1/16 of a pixel: the edge tests are then exact integer
	// sums, with no gaps or double-drawn pixels along shared edges
	int32_t x0 = (int32_t)(v0.x * 16), y0 = (int32_t)(v0.y * 16);
	int32_t x1 = (int32_t)(v1.x * 16), y1 = (int32_t)(v1.y * 16);
	int32_t x2 = (int32_t)(v2.x * 16), y2 = (int32_t)(v2.y * 16);

	// Twice the area. Negative: we see its back. Swap two corners so the
	// edge functions are positive inside either way.
	int32_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
	if (area == 0)
		return;
	if (area < 0) {
		struct vertex t = v1; v1 = v2; v2 = t;
		int32_t tx = x1, ty = y1;
		x1 = x2, y1 = y2, x2 = tx, y2 = ty;
		area = -area;
	}

	// The bounding box, cut to the screen
	int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	minx = minx < 0 ? 0 : minx >> 4;
	miny = miny < 0 ? 0 : miny >> 4;
	maxx = (maxx >> 4) + 1, maxy = (maxy >> 4) + 1;
	if (maxx > (int32_t)screen.width) maxx = screen.width;
	if (maxy > (int32_t)screen.height) maxy = screen.height;
	if (minx >= maxx || miny >= maxy)
		return;

	// Edge function for the edge a->b at point p: positive on the inside.
	// It changes by a fixed amount per pixel step in x or y, so each pixel
	// costs three additions.
	//   e(p) = (b.x - a.x)(p.y - a.y) - (b.y - a.y)(p.x - a.x)
	// "Top-left rule": a pixel exactly on an edge belongs to the triangle
	// only if it's a top or left edge (else the -1 makes it outside), so two
	// triangles sharing an edge never both draw it.
#define EDGE(ax, ay, bx, by, px, py) \
	((int64_t)((bx) - (ax)) * ((py) - (ay)) - (int64_t)((by) - (ay)) * ((px) - (ax)))
#define TOPLEFT(ax, ay, bx, by) (((ay) == (by) && (bx) < (ax)) || (by) < (ay) ? 0 : -1)
	int32_t px = minx * 16 + 8, py = miny * 16 + 8;   // the first pixel's centre
	int64_t e0row = EDGE(x1, y1, x2, y2, px, py) + TOPLEFT(x1, y1, x2, y2);
	int64_t e1row = EDGE(x2, y2, x0, y0, px, py) + TOPLEFT(x2, y2, x0, y0);
	int64_t e2row = EDGE(x0, y0, x1, y1, px, py) + TOPLEFT(x0, y0, x1, y1);
	// Steps per pixel (16 sub-pixel units)
	int64_t e0dx = -(int64_t)(y2 - y1) * 16, e0dy = (int64_t)(x2 - x1) * 16;
	int64_t e1dx = -(int64_t)(y0 - y2) * 16, e1dy = (int64_t)(x0 - x2) * 16;
	int64_t e2dx = -(int64_t)(y1 - y0) * 16, e2dy = (int64_t)(x1 - x0) * 16;

	// e0/area is corner 0's weight, and so on. Divide by z for perspective.
	float k0 = 1.0f / ((float)area * v0.z), k1 = 1.0f / ((float)area * v1.z),
	      k2 = 1.0f / ((float)area * v2.z);
	float r0 = v0.r * shade, g0 = v0.g * shade, b0 = v0.b * shade;
	float r1 = v1.r * shade, g1 = v1.g * shade, b1 = v1.b * shade;
	float r2 = v2.r * shade, g2 = v2.g * shade, b2 = v2.b * shade;

	for (int32_t y = miny; y < maxy; y++) {
		uint8_t *line = pic + y * screen.pitch;
		int64_t e0 = e0row, e1 = e1row, e2 = e2row;
		for (int32_t x = minx; x < maxx; x++) {
			if ((e0 | e1 | e2) >= 0) {   // all three >= 0: inside
				float q0 = (float)e0 * k0, q1 = (float)e1 * k1, q2 = (float)e2 * k2;
				float inv = 1.0f / (q0 + q1 + q2);
				put(line, (uint32_t)x,
				    (uint32_t)((q0 * r0 + q1 * r1 + q2 * r2) * inv),
				    (uint32_t)((q0 * g0 + q1 * g1 + q2 * g2) * inv),
				    (uint32_t)((q0 * b0 + q1 * b1 + q2 * b2) * inv));
				stats.pixels++;
			}
			e0 += e0dx, e1 += e1dx, e2 += e2dx;
		}
		e0row += e0dy, e1row += e1dy, e2row += e2dy;
	}
}

// ---- the loop on core 3 -----------------------------------------------------

static uint32_t button(const char *name)
{
	for (uint32_t i = 0; i < button_count; i++) {
		const char *a = buttons[i].name, *b = name;
		while (*a && *a == *b)
			a++, b++;
		if (*a == *b)
			return 1U << i;
	}
	return 0;
}

// Show `pic`, and wait until the VOP has started a frame with it
static int show(const uint8_t *pic)
{
	uintptr_t vop = screen.vop;
	wr(vop + (screen.window ? WIN1_MST : WIN0_MST), (uint32_t)(uintptr_t)pic);
	wr(vop + INTR_CLEAR, 1U << 16 | 1);   // forget the last frame start
	wr(vop + REG_CFG_DONE, 1);
	uint64_t t0 = ticks();
	while (!(rd(vop + INTR_STATUS) & 1U << 16))
		if (ticks() - t0 > 24000 * 50)   // 50 ms: a frame is ~17
			return -1;
	return 0;
}

static uint64_t stop_after;   // ticks; 0 = until told to stop
static void spin(void *unused)
{
	(void)unused;
	uint32_t bytes = screen.pitch * screen.height;
	uint32_t left = button("L"), right = button("R"), up = button("U"), down = button("D");
	make_background();

	// The triangle: equilateral, 1 unit from its centre to each corner,
	// red, green and blue corners
	static const float corner[3][3] = { { 0, 1, 0 }, { -0.866f, -0.5f, 0 }, { 0.866f, -0.5f, 0 } };
	static const float colour[3][3] = { { 255, 40, 40 }, { 40, 255, 40 }, { 60, 90, 255 } };
	float turn = 0, speed = 0.03f, tilt = 0.3f;
	// The eye: 90 degrees across the width... f = half the width / tan(45)
	float f = (float)screen.width / 2, cx = (float)screen.width / 2, cy = (float)screen.height / 2;

	uint64_t start = ticks();
	for (uint32_t n = 0; !stop && !(stop_after && ticks() - start > stop_after); n++) {
		uint8_t *pic = frame[n & 1];
		uint64_t t0 = ticks();
		copy(pic, background, bytes);

		// d-pad: left/right change the spin, up/down tilt it
		uint32_t held = buttons_pressed();
		if (held & left)  speed -= 0.002f;
		if (held & right) speed += 0.002f;
		if (held & up)    tilt -= 0.03f;
		if (held & down)  tilt += 0.03f;
		turn += speed;

		// Turn about the y axis (the spin), then the x axis (the tilt), then
		// push 2.5 units away and project
		float cs = cosine(turn), sn = sine(turn), ct = cosine(tilt), st = sine(tilt);
		struct vertex v[3];
		for (int i = 0; i < 3; i++) {
			float x = corner[i][0], y = corner[i][1], z = corner[i][2];
			float x1 = x * cs + z * sn, z1 = -x * sn + z * cs;   // about y
			float y2 = y * ct - z1 * st, z2 = y * st + z1 * ct;  // about x
			float depth = z2 + 2.5f;
			v[i] = (struct vertex){ cx + f * x1 / depth, cy - f * y2 / depth, depth,
						colour[i][0], colour[i][1], colour[i][2] };
		}
		// Which side faces us: the normal's z, from the turned corners. Its
		// back is drawn darker.
		float ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, bx = v[2].x - v[0].x, by = v[2].y - v[0].y;
		float facing = ax * by - ay * bx;
		uint64_t t1 = ticks();
		triangle(pic, v[0], v[1], v[2], facing < 0 ? 1.0f : 0.45f);
		uint64_t t2 = ticks();
		clean(pic, bytes);
		uint64_t t3 = ticks();
		if (show(pic))
			stats.late++;

		stats.frames++;
		stats.copy += t1 - t0;
		stats.draw += t2 - t1;
		stats.clean += t3 - t2;
		stats.elapsed = ticks() - start;
	}
	// Back to the console's own picture
	show((const uint8_t *)screen.fb);
}

// Test steps, on the calling core: 1 = background and one frame, not
// shown; 2 = also show it for a second, then the console again
volatile uint32_t render_stage;
const char *render_test(uint32_t step, uint64_t *took)
{
	uint32_t bytes = screen.pitch * screen.height;
	if (bytes > MAX_BYTES || !screen.vop)
		return "the screen is bigger than our buffers";
	uint64_t t0 = ticks();
	render_stage = 1;
	make_background();
	render_stage = 2;
	copy(frame[0], background, bytes);
	render_stage = 3;
	struct vertex v[3] = {
		{ 320, 100, 2.5f, 255, 40, 40 }, { 180, 380, 2.5f, 40, 255, 40 }, { 460, 380, 2.5f, 60, 90, 255 } };
	triangle(frame[0], v[0], v[1], v[2], 1.0f);
	render_stage = 4;
	clean(frame[0], bytes);
	render_stage = 5;
	took[0] = ticks() - t0;
	if (step == 3) {   // the whole loop, here, for 3 seconds
		stats = (struct render_stats){ 0 };
		stop = 0;
		stop_after = 24000000UL * 3;
		spin(0);
		stop_after = 0;
		return 0;
	}
	if (step >= 2) {
		int late = show(frame[0]);
		render_stage = 6;
		uint64_t t1 = ticks();
		while (ticks() - t1 < 24000000)
			;
		late |= show((const uint8_t *)screen.fb);
		render_stage = 7;
		if (late)
			return "the VOP didn't start a frame within 50 ms";
	}
	return 0;
}

const char *render_start(void)
{
	if (running)
		return "already spinning";
	if (screen.pitch * screen.height > MAX_BYTES || !screen.vop)
		return "the screen is bigger than our buffers";
	const char *err = cores_start();
	if (err)
		return err;
	if (core_busy(CORE))
		return "core 3 is busy";
	stats = (struct render_stats){ 0 };
	stop = 0;
	running = 1;
	core_run(CORE, spin, 0);
	return 0;
}

void render_stop(void)
{
	if (!running)
		return;
	stop = 1;
	core_wait(CORE);
	running = 0;
}

int render_running(void) { return running; }
struct render_stats render_stats(void) { return stats; }
