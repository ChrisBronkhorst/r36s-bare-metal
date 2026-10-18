// Register map from Linux's rk808.h, rk808-regulator.c and rk817_charger.c.
// Read-only so far: writing here changes real voltages (vdd_arm is the CPU's).

#include "i2c.h"
#include "rk817.h"

#define ID_MSB       0xed
#define BAT_VOL_H    0x78   // battery voltage, ADC counts (2 bytes, big-endian)
#define BAT_CUR_H    0x7a   // battery current, signed ADC counts
#define USB_VOL_H    0x7e   // charging port voltage
#define VCALIB0_H    0x93   // factory calibration points for the voltage ADC
#define VCALIB1_H    0x95
#define ADC_CONFIG0  0x50   // which measurements are switched on
#define USB_VOL_ADC_EN (1U << 4)
#define CHRG_STS     0xeb
#define SYS_STS      0xf0
#define PLUG_IN      (1U << 6)
#define POWER_EN(i)  (0xb1 + (i))   // on/off: 4 outputs per register, bits 0-3
#define BUCK_VSEL(i) (0xbb + 3 * (i))
#define LDO_VSEL(i)  (0xcc + 2 * (i))

static int read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	return i2c_read(I2C0, RK817_ADDR, reg, buf, len);
}

static int read16(uint8_t reg, uint32_t *value)
{
	uint8_t b[2];
	int err = read(reg, b, 2);
	*value = (uint32_t)b[0] << 8 | b[1];
	return err;
}

int rk817_id(uint32_t *id)
{
	return read16(ID_MSB, id);
}

int rk817_battery(struct battery *b)
{
	uint32_t vcalib0, vcalib1, vol, cur, usb;
	uint8_t sts, sys, adc;
	int err = read16(VCALIB0_H, &vcalib0) | read16(VCALIB1_H, &vcalib1) |
		read16(BAT_VOL_H, &vol) | read16(BAT_CUR_H, &cur) |
		read16(USB_VOL_H, &usb) | read(CHRG_STS, &sts, 1) |
		read(SYS_STS, &sys, 1) | read(ADC_CONFIG0, &adc, 1);
	if (err)
		return err;

	// The ADC is calibrated at the factory: VCALIB0 and VCALIB1 are what it
	// reads at 2.300 V and 4.025 V. Draw a straight line through them.
	// (k is in microvolts per count, b in millivolts, as in Linux.)
	uint32_t span = vcalib1 > vcalib0 ? vcalib1 - vcalib0 : 1;
	int32_t k = (4025 - 2300) * 1000 / span;
	int32_t c = 4025 - k * (int32_t)vcalib1 / 1000;
	b->mv = k * (int32_t)vol / 1000 + c;

	// Current: the voltage across a 10 milliohm resistor in the battery
	// line, 172 microamps per count.
	b->ma = (int16_t)cur * 172 / 1000;

	// Plugged in: a status bit. The port's voltage is only measured if that
	// ADC channel is on, and U-Boot may leave it off.
	b->plugged_in = sys & PLUG_IN ? 1 : 0;
	b->charger_mv = adc & USB_VOL_ADC_EN && usb > 1 ?
		(k * (int32_t)usb / 1000 + c) * 60 / 46 : 0;   // through a 46:60 divider
	b->status = sts >> 4 & 7;
	return 0;
}

const char *rk817_charge_status(uint32_t status)
{
	static const char *const names[8] = {
		"not charging", "reviving battery", "trickle charging", "charging",
		"charged", "port over-voltage", "battery temp error", "charge timeout" };
	return names[status & 7];
}

int rk817_rail_enable(uint32_t rail, int on)
{
	// Each on/off register holds 4 outputs, in bits 0-3. Bits 4-7 are a
	// write mask: only outputs whose mask bit is set change. So we can
	// write one output without reading the register first.
	uint32_t bit = rail % 4;
	uint8_t value = (uint8_t)(1U << (bit + 4) | (on ? 1U << bit : 0));
	return i2c_write(I2C0, RK817_ADDR, POWER_EN(rail / 4), &value, 1);
}

int rk817_ldo_set_mv(uint32_t ldo, uint32_t mv)
{
	if (ldo < 1 || ldo > 9 || mv < 600 || mv > 3400)
		return -1;
	uint8_t sel = (uint8_t)((mv - 600) / 25);
	return i2c_write(I2C0, RK817_ADDR, LDO_VSEL(ldo - 1), &sel, 1);
}

int rk817_rails(struct rail rails[RK817_RAILS])
{
	static const char *const outputs[RK817_RAILS] = {
		"DCDC1", "DCDC2", "DCDC3", "DCDC4",
		"LDO1", "LDO2", "LDO3", "LDO4", "LDO5", "LDO6", "LDO7", "LDO8", "LDO9" };
	static const char *const names[RK817_RAILS] = {
		"vdd_logic", "vdd_arm", "vcc_ddr", "vcc_3v3",
		"vcc_1v0", "vcc1v8_soc", "vdd1v0_soc", "vcc3v3_pmu",
		"vccio_sd", "vcc_sd", "vcc_backlight", "vcc_lcd", "(unused)" };

	uint8_t en[4], vsel[18];
	int err = read(POWER_EN(0), en, 4) | read(BUCK_VSEL(0), vsel, 1) |
		read(BUCK_VSEL(1), vsel + 1, 1) | read(BUCK_VSEL(2), vsel + 2, 1) |
		read(BUCK_VSEL(3), vsel + 3, 1) | read(LDO_VSEL(0), vsel + 4, 17);
	if (err)
		return err;

	for (uint32_t i = 0; i < RK817_RAILS; i++) {
		rails[i].output = outputs[i];
		rails[i].name = names[i];
		rails[i].on = en[i / 4] >> (i % 4) & 1;
		if (i < 4) {
			// DC-DC: 0.5 V + 12.5 mV steps up to 1.5 V, then 100 mV steps
			uint32_t sel = vsel[i] & 0x7f;
			rails[i].mv = sel <= 80 ? 500 + sel * 25 / 2 : 1600 + (sel - 81) * 100;
			if (i == 2)
				rails[i].mv = 0;   // vcc_ddr is set by resistors, not this
		} else {
			// LDO: 0.6 V + 25 mV steps (their registers are 2 apart)
			uint32_t sel = vsel[4 + (i - 4) * 2] & 0x7f;
			rails[i].mv = 600 + sel * 25;
		}
	}
	return 0;
}
