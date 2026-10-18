// Who is on the I2C buses?
//
// A chip that hears its own address pulls the data line low for one clock:
// the "acknowledge". So trying all 112 usable addresses, and noting which
// ones are acknowledged, lists every chip on a bus: also ones the device
// tree doesn't mention. (This is what Linux's `i2cdetect` does.) For each
// address we ask for one byte from register 0, which changes nothing.
//
// The device tree lists the power chip (0x20) on I2C0, and nothing at all on
// I2C1, though that bus is switched on. A camera sensor, a touch screen, or
// anything else hidden on the board would show up as an extra address here.
//
// I2C1 isn't set up by U-Boot, so we set it up just for the scan: its clocks,
// its two pins (GPIO0 C2 and C3, which the device tree gives to I2C1) and
// its speed. Afterwards the pins go back to how they were.

#include "console.h"
#include "dump.h"
#include "i2c.h"
#include "scan.h"

#define I2C1    0xff190000UL
#define CRU     0xff2b0000UL
#define PMUCRU  0xff2bc000UL
#define PMUGRF  0xff010000UL   // pin functions for GPIO0
#define GPIO0   0xff040000UL

#define CLKSEL(n)  (CRU + 0x100 + 4 * (n))
#define CLKGATE(n) (CRU + 0x200 + 4 * (n))
#define GPIO0B_IOMUX (PMUGRF + 0x04)   // 2 bits a pin
#define GPIO0C_IOMUX (PMUGRF + 0x08)

static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

// Rockchip "write mask" registers: the top 16 bits say which of the bottom
// 16 to change, so you never have to read first.
static void masked(uintptr_t a, uint32_t mask, uint32_t value)
{
	wr(a, mask << 16 | (value & mask));
}

// A grid like i2cdetect's: rows 0x00-0x70, columns 0-f
static uint32_t scan(uintptr_t bus, uint32_t row)
{
	uint32_t found = 0;
	console_goto(0, row);
	console_color(GREY);
	kputs("   0 1 2 3 4 5 6 7 8 9 a b c d e f");
	for (uint32_t hi = 0; hi < 8; hi++) {
		console_goto(0, row + 1 + hi);
		console_color(GREY);
		kprintf("%xx", hi);
		for (uint32_t lo = 0; lo < 16; lo++) {
			uint32_t addr = hi << 4 | lo;
			if (addr < 0x08 || addr > 0x77) {   // reserved addresses
				kputs("  ");
				continue;
			}
			uint8_t b;
			int err = i2c_read(bus, addr, 0, &b, 1);
			if (err == 0) {
				console_color(GREEN);
				kprintf("%02x", addr);
				found++;
			} else if (err == -1) {   // no acknowledge: nobody there
				console_color(GREY);
				kputs(" .");
			} else {                  // the bus itself didn't work
				console_color(YELLOW);
				kputs(" ?");
			}
		}
	}
	return found;
}

static void scan_i2c1(uint32_t row)
{
	console_goto(0, row);
	console_color(YELLOW);
	kputs("I2C1  ");
	console_color(GREY);
	// Only if I2C0's pins read as expected: then we know the layout of the
	// pin registers, and the write below lands where we mean it to.
	if ((rd(GPIO0B_IOMUX) & 0xf) != 0x5) {
		kputs("not scanned: pin check failed");
		return;
	}

	// Its clocks: the bus clock (gate 12 bit 1) and the register clock
	// (gate 14 bit 11). A set gate bit means stopped.
	uint32_t gates = (rd(CLKGATE(12)) >> 1 & 1) | (rd(CLKGATE(14)) >> 11 & 1) << 1;
	masked(CLKGATE(12), 1U << 1, 0);
	masked(CLKGATE(14), 1U << 11, 0);
	// and GPIO0's, to look at the pins
	uint32_t gpio_gate = rd(PMUCRU + 0x80) >> 6 & 1;
	masked(PMUCRU + 0x80, 1U << 6, 0);

	// If something drives those pins as outputs, they aren't free: leave them.
	uint32_t outputs = rd(GPIO0 + 0x04) >> 18 & 3;
	uint32_t mux = rd(GPIO0C_IOMUX);
	if (outputs) {
		kputs("not scanned: pins are outputs");
	} else {
		uint32_t was = mux >> 4 & 0xf;
		kprintf("pins were %s inputs", was == 0x5 ? "I2C" : was ? "other" : "GPIO");
		// Bus speed: 100 kHz. SCL = clock / (16 x (div + 1)).
		uint32_t sel = rd(CLKSEL(49)) >> 8;
		uint32_t khz = (sel >> 7 & 1 ? 24000 : gpll_khz()) / ((sel & 0x7f) + 1);
		uint32_t div = (khz + 1599) / 1600 - 1;
		wr(I2C1 + 0x04, div << 16 | div);
		masked(GPIO0C_IOMUX, 0xf0, 0x50);   // C2, C3: function 1, I2C1
		scan(I2C1, row + 1);
		masked(GPIO0C_IOMUX, 0xf0, mux);    // put the pins back
	}
	masked(PMUCRU + 0x80, 1U << 6, gpio_gate << 6);
	masked(CLKGATE(12), 1U << 1, (gates & 1) << 1);
	masked(CLKGATE(14), 1U << 11, (gates >> 1 & 1) << 11);
}

void i2c_scan_page(void)
{
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("Who answers on I2C?\n");

	console_goto(0, 1);
	console_color(YELLOW);
	kputs("I2C0");
	console_color(GREY);
	// Sanity check on our reading of the pin registers: U-Boot set I2C0's
	// pins, GPIO0 B0 and B1, to function 1.
	uint32_t i2c0_pins = rd(GPIO0B_IOMUX) & 0xf;
	kprintf("  pins B0 B1: function %u %u\n", i2c0_pins & 3, i2c0_pins >> 2);
	scan(I2C0, 2);
	scan_i2c1(11);

	console_goto(0, 22);
	console_color(YELLOW);
	kputs("A: button test");
}
