// The chip's temperature sensor, the "TSADC" (temperature sensor + ADC):
// two diodes on the die, one by the CPU and one by the GPU, whose voltage
// changes with temperature, measured by a slow 12-bit converter.
//
// Set-up, as Linux does it (rockchip_thermal.c, "px30"):
//   - the converter's clock: 50 kHz. It comes up at 12 MHz, far too fast:
//     it starts a measurement and never finishes ("busy" forever).
//   - a reset pulse, to clear that stuck state
//   - "auto mode": it measures both channels every 250 clock cycles (5 ms)
//     by itself; we just read the latest result whenever we like
//
// And one safety net, also as Linux: if either channel reads over 95 C four
// times in a row, the sensor resets the whole chip itself ("TSHUT"). No
// software involved, so it works even if the kernel has hung.

#include <stdint.h>
#include "thermal.h"
#include "sysreg.h"

#define TSADC        0xff280000UL
#define AUTO_CON     (TSADC + 0x04)
#define INT_EN       (TSADC + 0x08)
#define DATA(ch)     (TSADC + 0x20 + 4 * (ch))
#define COMP_SHUT(ch) (TSADC + 0x40 + 4 * (ch))
#define INT_DEBOUNCE (TSADC + 0x60)
#define SHUT_DEBOUNCE (TSADC + 0x64)
#define AUTO_PERIOD  (TSADC + 0x68)
#define AUTO_PERIOD_HT (TSADC + 0x6c)

#define CRU          0xff2b0000UL
#define CLKSEL54     (CRU + 0x100 + 54 * 4)   // clk_tsadc divider, from 24 MHz
#define SOFTRST10    (CRU + 0x300 + 10 * 4)   // bit 7: register side, bit 8: converter
#define GRF_SOC_CON2 0xff140408UL

#define AUTO_EN      (1U << 0)
#define Q_SEL        (1U << 1)   // codes count up with temperature
#define SRC_EN(ch)   (1U << (4 + (ch)))
#define SHUT_2CRU(ch) (1U << (8 + (ch)))   // over the limit: reset the chip

#define SHUT_CODE    586   // 95 C, from the table below

static void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

// Code -> temperature, measured by Rockchip (Linux's rk3328_code_table,
// which the PX30 shares): every 5 C
static const struct { uint16_t code; int16_t c; } table[] = {
	{296, -40}, {304, -35}, {313, -30}, {322, -25}, {331, -20}, {340, -15},
	{349, -10}, {359, -5}, {368, 0}, {378, 5}, {388, 10}, {398, 15},
	{408, 20}, {418, 25}, {429, 30}, {440, 35}, {451, 40}, {462, 45},
	{473, 50}, {485, 55}, {496, 60}, {508, 65}, {521, 70}, {533, 75},
	{546, 80}, {559, 85}, {572, 90}, {586, 95}, {600, 100}, {614, 105},
	{629, 110}, {644, 115}, {659, 120}, {675, 125},
};
#define ENTRIES (sizeof table / sizeof table[0])

void thermal_init(void)
{
	wr(CLKSEL54, 0x7ffU << 16 | (24000000 / 50000 - 1));   // 50 kHz
	wr(SOFTRST10, 3U << (7 + 16) | 3U << 7);   // into reset...
	uint64_t t = READ_SYSREG(cntpct_el0) + 24 * 20;
	while (READ_SYSREG(cntpct_el0) < t)
		;
	wr(SOFTRST10, 3U << (7 + 16));             // ...and out (20 us)
	wr(GRF_SOC_CON2, 1U << (1 + 16) | 1U << 1);   // as Linux; unexplained there

	wr(AUTO_PERIOD, 250);       // a reading every 5 ms...
	wr(AUTO_PERIOD_HT, 50);     // ...every 1 ms once over a limit
	wr(INT_DEBOUNCE, 4);
	wr(SHUT_DEBOUNCE, 4);       // 4 readings in a row before acting
	for (uint32_t ch = 0; ch < 2; ch++)
		wr(COMP_SHUT(ch), SHUT_CODE);
	wr(INT_EN, SHUT_2CRU(0) | SHUT_2CRU(1));
	wr(AUTO_CON, SRC_EN(0) | SRC_EN(1) | Q_SEL | AUTO_EN);
}

int thermal_read(uint32_t channel, int32_t *tenths)
{
	uint32_t code = rd(DATA(channel)) & 0xfff;
	if (channel > 1 || code < table[0].code || code > table[ENTRIES - 1].code)
		return -1;   // nothing measured yet, or off the table
	uint32_t i = 1;
	while (table[i].code < code)
		i++;
	// Straight line between the two table entries either side
	int32_t c0 = table[i - 1].c * 10, c1 = table[i].c * 10;
	int32_t span = table[i].code - table[i - 1].code;
	*tenths = c0 + (c1 - c0) * (int32_t)(code - table[i - 1].code) / span;
	return 0;
}
