// The two analogue sticks.
//
// Each stick is two potentiometers (x and y): a voltage between 0 and
// 1.8 V that follows the stick. The chip measures voltages with its SARADC
// (successive-approximation analogue-to-digital converter): 10 bits, 0 to
// 1023. But all four voltages arrive on ONE input, channel 1, through a
// 4-way analogue switch on the board. Two GPIO pins (A, B) choose which of
// the four it passes; a third (EN) switches it on. (Device tree: the
// "odroidgo3-joypad" node, amux-a/b/en-gpios = GPIO3 pins 11, 8, 13.)
// So to read a stick: set A and B, wait a moment for the voltage to settle,
// then convert.

#include <stdint.h>
#include "gpio.h"
#include "joystick.h"
#include "sysreg.h"

#define SARADC   0xff288000UL
#define ADC_DATA 0x00   // the last result, 10 bits
#define ADC_CTRL 0x08   // bit 3 power on (and go), bits 0-2 channel, bit 5 interrupt
                        // on, bit 6 done (only set with bit 5: we don't let the GIC
                        // pass the interrupt on, we just poll the bit)
#define ADC_DLY  0x0c   // clocks between power-on and the start
#define CHANNEL  1

#define PIN_A  11
#define PIN_B  8
#define PIN_EN 13

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint64_t ticks(void) { return READ_SYSREG(cntpct_el0); }

static void wait_us(uint32_t us)
{
	uint64_t t0 = ticks();
	while (ticks() - t0 < (uint64_t)us * 24)
		;
}

// One conversion: about 13 clocks of the ADC's own clock. -1 if it never ends.
int32_t adc_read(uint32_t channel)
{
	wr(SARADC + ADC_CTRL, 0);   // off, and forget the last "done"
	wr(SARADC + ADC_DLY, 8);
	wr(SARADC + ADC_CTRL, 1U << 5 | 1U << 3 | (channel & 7));
	uint64_t t0 = ticks();
	while (!(rd(SARADC + ADC_CTRL) & 1U << 6))
		if (ticks() - t0 > 24 * 1000)   // 1 ms
			return -1;
	int32_t v = (int32_t)(rd(SARADC + ADC_DATA) & 0x3ff);
	wr(SARADC + ADC_CTRL, 0);
	return v;
}

// Switch position 0-3, with the EN pin at `en`: the raw level on the pins
// (the device tree says "active low"; we try both to see which works)
int32_t joy_raw(uint32_t position, int en)
{
	gpio_output(3, PIN_A, position & 1);
	gpio_output(3, PIN_B, position >> 1 & 1);
	gpio_output(3, PIN_EN, en);
	wait_us(20);
	return adc_read(CHANNEL);
}

// One axis, -512 to 511 from its resting point, 0 inside a small dead zone
// (a stick at rest never reads exactly the same). The switch's positions
// (EN low: it's "active low"), as the Linux driver names them:
//   0 right stick y, 1 right stick x, 2 left stick y, 3 left stick x
static int32_t rest[4];
static int resting;

void joy_rest(void)
{
	for (uint32_t p = 0; p < 4; p++)
		rest[p] = joy_raw(p, 0);
	resting = 1;
}

int32_t joy_axis(uint32_t position)
{
	if (!resting)
		joy_rest();
	int32_t v = joy_raw(position, 0);
	if (v < 0)
		return 0;
	v -= rest[position];
	if (v > -40 && v < 40)
		return 0;
	return v < -512 ? -512 : v > 511 ? 511 : v;
}
