#pragma once
#include <stdint.h>

// The RK817 power chip ("PMIC"): it makes every voltage on the board from the
// battery, charges the battery, and measures it. It sits on I2C bus 0 at
// address 0x20. (It's also the console's audio codec.)

#define RK817_ADDR 0x20

int rk817_id(uint32_t *id);   // 0 if it answered; the ID should be 0x817x

struct battery {
	uint32_t mv;           // battery voltage
	int32_t ma;            // current: + into the battery (charging), - out
	int plugged_in;        // is a charger connected?
	uint32_t charger_mv;   // voltage at the charging port, 0 if not measured
	uint32_t status;       // 0-7, see rk817_charge_status()
};
int rk817_battery(struct battery *b);
const char *rk817_charge_status(uint32_t status);

// The chip's power outputs: 4 DC-DC converters, 9 LDOs, and a 5 V boost
struct rail {
	const char *output, *name;   // e.g. "LDO8", "vcc_lcd" (from the device tree)
	int on;
	uint32_t mv;                 // 0 if it has no programmable voltage
};
#define RK817_RAILS 13
int rk817_rails(struct rail rails[RK817_RAILS]);

// Switch output `rail` (0-12, as in rk817_rails) on or off. Its voltage
// stays as it is.
int rk817_rail_enable(uint32_t rail, int on);
#define RAIL_VCC_BACKLIGHT 10   // LDO7
#define RAIL_VCC_LCD 11         // LDO8, the panel's power

// Set LDO n's (1-9) voltage: 0.6 to 3.4 V in 25 mV steps
int rk817_ldo_set_mv(uint32_t ldo, uint32_t mv);
