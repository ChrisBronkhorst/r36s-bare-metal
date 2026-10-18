// The Rockchip I2C controller ("rk3x"), layout from Linux's i2c-rk3x.c.
//
// The controller does the bit-level work: START, the address byte, waiting
// for each acknowledge, STOP. We tell it what to do in steps and wait for
// the "done" flags in IPD. U-Boot already set the bus speed (CLKDIV, and
// timing tweaks in CON bits 8-15), the pins and the clock: we keep those.
//
// We poll instead of using the controller's interrupt: transfers take tens
// of microseconds, and polling keeps the driver short.

#include "i2c.h"
#include "sysreg.h"

#define CON     0x00
#define CLKDIV  0x04
#define MRXADDR 0x08   // chip address, for "register read" mode
#define MRXRADDR 0x0c  // register address, for "register read" mode
#define MTXCNT  0x10   // bytes to send (writing it starts sending)
#define MRXCNT  0x14   // bytes to receive (writing it starts receiving)
#define IEN     0x18
#define IPD     0x1c   // what has happened; write 1 to clear
#define TXDATA  0x100  // 32 bytes to send
#define RXDATA  0x200  // 32 bytes received

#define CON_EN       (1U << 0)
#define CON_MOD(m)   ((uint32_t)(m) << 1)
#define MOD_TX       0   // just send bytes
#define MOD_REG_RX   1   // send chip + register, then restart and receive
#define CON_START    (1U << 3)
#define CON_STOP     (1U << 4)
#define CON_LASTACK  (1U << 5)   // NACK the last byte: "that's all I want"
#define CON_ACTACK   (1U << 6)   // stop if the chip doesn't acknowledge
#define CON_TUNING   0xff00U     // U-Boot's timing settings: keep

#define INT_MBTF   (1U << 2)   // finished sending
#define INT_MBRF   (1U << 3)   // finished receiving
#define INT_START  (1U << 4)   // START sent
#define INT_STOP   (1U << 5)   // STOP sent
#define INT_NAK    (1U << 6)   // the chip didn't acknowledge
#define INT_ALL    0x7fU

#define VALID(n)   (1U << (24 + (n)))   // MRX(R)ADDR: byte n is used

static inline uint32_t rd(uintptr_t bus, uint32_t reg) { return *(volatile uint32_t *)(bus + reg); }
static inline void wr(uintptr_t bus, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(bus + reg) = v; }

// Wait for one of `bits` in IPD; give up after 10 ms (at 400 kHz a byte
// takes about 25 microseconds).
static uint32_t wait_for(uintptr_t bus, uint32_t bits)
{
	uint64_t start = READ_SYSREG(cntpct_el0);
	uint64_t freq = READ_SYSREG(cntfrq_el0);
	uint64_t limit = (freq ? freq : 24000000) / 100;   // in case nobody set it
	for (;;) {
		uint32_t ipd = rd(bus, IPD);
		if (ipd & bits)
			return ipd;
		if (READ_SYSREG(cntpct_el0) - start > limit)
			return 0;
	}
}

static void stop(uintptr_t bus)
{
	wr(bus, IPD, INT_ALL);
	wr(bus, CON, (rd(bus, CON) & ~CON_START) | CON_STOP);
	wait_for(bus, INT_STOP);
	wr(bus, IPD, INT_ALL);
	wr(bus, CON, rd(bus, CON) & CON_TUNING);   // controller off
}

static int start(uintptr_t bus, uint32_t mode)
{
	wr(bus, IEN, INT_ALL);   // flags show up in IPD (the GIC ignores them)
	wr(bus, IPD, INT_ALL);
	wr(bus, CON, (rd(bus, CON) & CON_TUNING) | CON_EN | CON_MOD(mode) | CON_START | CON_ACTACK);
	if (!wait_for(bus, INT_START)) {
		stop(bus);
		return -2;
	}
	wr(bus, IPD, INT_START);
	wr(bus, CON, rd(bus, CON) & ~CON_START);
	return 0;
}

int i2c_read(uintptr_t bus, uint8_t chip, uint8_t reg, uint8_t *buf, uint32_t len)
{
	if (len == 0 || len > 32)
		return -2;
	// "Register read": the controller sends [chip, write] [reg], then a
	// repeated START and [chip, read], then receives.
	wr(bus, MRXADDR, (uint32_t)chip << 1 | VALID(0));
	wr(bus, MRXRADDR, reg | VALID(0));
	int err = start(bus, MOD_REG_RX);
	if (err)
		return err;

	wr(bus, CON, rd(bus, CON) | CON_LASTACK);
	wr(bus, MRXCNT, len);   // go
	uint32_t ipd = wait_for(bus, INT_MBRF | INT_NAK);
	if (!(ipd & INT_MBRF)) {
		stop(bus);
		return ipd & INT_NAK ? -1 : -2;
	}
	uint32_t word = 0;
	for (uint32_t i = 0; i < len; i++) {
		if (i % 4 == 0)
			word = rd(bus, RXDATA + i);
		buf[i] = word >> (i % 4 * 8);
	}
	stop(bus);
	return 0;
}

int i2c_write(uintptr_t bus, uint8_t chip, uint8_t reg, const uint8_t *buf, uint32_t len)
{
	if (len > 30)   // + chip address + register = 32
		return -2;
	int err = start(bus, MOD_TX);
	if (err)
		return err;

	// In plain send mode the first byte must be the chip address (write).
	uint8_t bytes[32] = { (uint8_t)(chip << 1), reg };
	for (uint32_t i = 0; i < len; i++)
		bytes[2 + i] = buf[i];
	for (uint32_t i = 0; i < len + 2; i += 4)
		wr(bus, TXDATA + i, bytes[i] | bytes[i + 1] << 8 | bytes[i + 2] << 16 |
			(uint32_t)bytes[i + 3] << 24);
	wr(bus, MTXCNT, len + 2);   // go
	uint32_t ipd = wait_for(bus, INT_MBTF | INT_NAK);
	stop(bus);
	if (ipd & INT_MBTF)
		return 0;
	return ipd & INT_NAK ? -1 : -2;
}
