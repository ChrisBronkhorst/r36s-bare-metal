#pragma once
// Software 3D on core 3: a spinning triangle (render.c)
#include <stdint.h>

struct render_stats {
	uint64_t frames, late;        // frames shown; times the VOP didn't answer
	uint64_t pixels;              // triangle pixels drawn
	uint64_t copy, draw, clean;   // ticks spent clearing, drawing, writing out
	uint64_t elapsed;             // ticks since the start
};

const char *render_start(void);   // 0, or why not
void render_stop(void);           // and the console's picture comes back
int render_running(void);
const char *render_test(uint32_t step, uint64_t *took);   // on this core
struct render_stats render_stats(void);
