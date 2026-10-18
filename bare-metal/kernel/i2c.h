#pragma once
#include <stdint.h>

// I2C: a two-wire bus (clock + data) for talking to small chips on the board.
// The SoC is the "master" and starts every transfer; each chip on the bus has
// a 7-bit address and answers only to that. Chips are organised as numbered
// 8-bit registers, so a transfer is "chip X: register R: these bytes".

#define I2C0 0xff180000UL   // the power chip's bus

// 0 on success, -1 if no chip answered, -2 if the controller timed out.
// At most 32 bytes at a time.
int i2c_read(uintptr_t bus, uint8_t chip, uint8_t reg, uint8_t *buf, uint32_t len);
int i2c_write(uintptr_t bus, uint8_t chip, uint8_t reg, const uint8_t *buf, uint32_t len);
