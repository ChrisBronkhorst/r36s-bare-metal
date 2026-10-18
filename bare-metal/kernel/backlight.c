// PWM1, the "rockchip,rk3328-pwm" layout (from U-Boot's rk_pwm.c). U-Boot
// already set the period, clock and pin, so we only change the duty: how many
// clock ticks of each period the output is "active".

#include "backlight.h"

#define PWM1 0xff200010UL
#define PWM_PERIOD 0x04
#define PWM_DUTY   0x08
#define PWM_CTRL   0x0c
#define CTRL_DUTY_POSITIVE (1U << 3)   // active = high (else active = low)
#define CTRL_LOCK          (1U << 6)   // hold changes until unlocked

static inline uint32_t read32(uintptr_t addr) { return *(volatile uint32_t *)addr; }
static inline void write32(uintptr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

static uint32_t period;

void backlight_init(void)
{
	period = read32(PWM1 + PWM_PERIOD);
}

uint32_t backlight_period(void)
{
	return period;
}

uint32_t backlight_get(void)
{
	return period ? (read32(PWM1 + PWM_DUTY) * 100 + period / 2) / period : 0;
}

void backlight_set(uint32_t percent)
{
	if (!period)
		return;
	if (percent > 100)
		percent = 100;
	uint32_t ctrl = read32(PWM1 + PWM_CTRL);
	// Lock, change, unlock: the new duty starts cleanly at the next period
	// instead of in the middle of one (which could flicker).
	write32(PWM1 + PWM_CTRL, ctrl | CTRL_LOCK);
	write32(PWM1 + PWM_DUTY, period * percent / 100);
	write32(PWM1 + PWM_CTRL, ctrl & ~CTRL_LOCK);
}
