// GPIO: reading and driving the chip's pins.
//
// Two separate blocks are involved:
//  - the GRF ("general register files") decides what each pin is for (GPIO,
//    or UART, I2C, ...) and whether it has a pull-up or pull-down resistor.
//    Bank 0 is configured in the PMUGRF instead (it stays powered in sleep).
//  - the GPIO controller itself: direction (in/out), output level, and the
//    live input level of every pin.
// Register layout from Linux's pinctrl-rockchip.c and gpio-rockchip.c (PX30).

#include "gpio.h"

#define PMUGRF 0xff010000UL
#define GRF    0xff140000UL

static const uintptr_t gpio_base[4] = {
	0xff040000UL, 0xff250000UL, 0xff260000UL, 0xff270000UL,
};

// GPIO controller registers
#define GPIO_DR   0x00   // output level, bit per pin
#define GPIO_DDR  0x04   // direction: 1 = output
#define GPIO_EXT  0x50   // what the pins actually read right now

// GPIO interrupt registers, a bit per pin
#define GPIO_INTEN    0x30   // 1 = this pin can interrupt
#define GPIO_INTMASK  0x34   // 1 = ...but not right now
#define GPIO_INTTYPE  0x38   // 0 = while at a level, 1 = on an edge
#define GPIO_POLARITY 0x3c   // 0 = low (or falling), 1 = high (or rising)
#define GPIO_STATUS   0x40   // which pins are interrupting

static inline uint32_t read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

// GRF registers use a "write mask": the top 16 bits say which of the bottom
// 16 bits this write changes. So we can set one pin's field without reading
// the register first, and without disturbing the other pins in it.
static void grf_set(uintptr_t reg, uint32_t bit, uint32_t mask, uint32_t value)
{
	write32(reg, mask << (bit + 16) | (value & mask) << bit);
}

// Pin function 0 is plain GPIO on every pin.
static void mux_gpio(uint32_t bank, uint32_t pin)
{
	if (bank == 0)   // 2 bits per pin, 8 pins per register
		grf_set(PMUGRF + pin / 8 * 4, pin % 8 * 2, 0x3, 0);
	else             // 4 bits per pin, 4 pins per register, 0x20 per bank
		grf_set(GRF + (bank - 1) * 0x20 + pin / 4 * 4, pin % 4 * 4, 0xf, 0);
}

void gpio_input_pullup(uint32_t bank, uint32_t pin)
{
	mux_gpio(bank, pin);
	// pull: 2 bits per pin, 8 pins per register; 1 = pull-up
	uintptr_t pull = bank == 0 ? PMUGRF + 0x10 : GRF + 0x50 + bank * 0x10;
	grf_set(pull + pin / 8 * 4, pin % 8 * 2, 0x3, 1);
	uintptr_t base = gpio_base[bank];
	write32(base + GPIO_DDR, read32(base + GPIO_DDR) & ~(1U << pin));
}

void gpio_output(uint32_t bank, uint32_t pin, int on)
{
	mux_gpio(bank, pin);
	uintptr_t base = gpio_base[bank];
	uint32_t level = read32(base + GPIO_DR);
	write32(base + GPIO_DR, on ? level | 1U << pin : level & ~(1U << pin));
	write32(base + GPIO_DDR, read32(base + GPIO_DDR) | 1U << pin);
}

// This controller can't interrupt on "either edge". So we use a trick
// (Linux does too): interrupt while the pin is at the level it's NOT at now.
// When it changes, it interrupts; the handler flips the polarity to the new
// opposite, which stops the interrupt until the next change.
static void set_bit(uintptr_t reg, uint32_t pin, int on)
{
	uint32_t v = read32(reg);
	write32(reg, on ? v | 1U << pin : v & ~(1U << pin));
}

void gpio_watch(uint32_t bank, uint32_t pin)
{
	uintptr_t base = gpio_base[bank];
	int level = read32(base + GPIO_EXT) >> pin & 1;
	set_bit(base + GPIO_INTTYPE, pin, 0);         // level-triggered
	set_bit(base + GPIO_POLARITY, pin, !level);   // the level it isn't at
	set_bit(base + GPIO_INTMASK, pin, 0);
	set_bit(base + GPIO_INTEN, pin, 1);
}

uint32_t gpio_changed(uint32_t bank)
{
	uintptr_t base = gpio_base[bank];
	uint32_t changed = read32(base + GPIO_STATUS);
	uint32_t level = read32(base + GPIO_EXT);
	uint32_t polarity = read32(base + GPIO_POLARITY);
	// for the pins that changed: now interrupt on the opposite of the new level
	write32(base + GPIO_POLARITY, (polarity & ~changed) | (~level & changed));
	return changed;
}

uint32_t gpio_read(uint32_t bank)
{
	return read32(gpio_base[bank] + GPIO_EXT);
}
