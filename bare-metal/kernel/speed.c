#include "screen.h"
#include "speed.h"
#include "sysreg.h"

extern char _start[];

static volatile uint64_t buffer[8192];   // 64 KB
static volatile uint32_t sink;           // so the compiler can't skip the work

static uint64_t now(void)
{
	__asm__ volatile("isb");   // don't let the CPU read the clock early
	return READ_SYSREG(cntpct_el0);
}

// CRC-32, one bit at a time: lots of small instructions in a tight loop
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t crc = ~0U;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}

void speed_test(struct speed *s, uint32_t screen_rgb)
{
	uint64_t t = now();
	sink = crc32((const uint8_t *)_start, 4096);
	s->code = now() - t;

	t = now();
	for (uint32_t i = 0; i < 8192; i++)
		buffer[i] = i;
	uint64_t sum = 0;
	for (uint32_t i = 0; i < 8192; i++)
		sum += buffer[i];
	sink = (uint32_t)sum;
	s->ram = now() - t;

	t = now();
	rect(0, 0, screen.width, screen.height, screen_rgb);
	s->screen = now() - t;
}
