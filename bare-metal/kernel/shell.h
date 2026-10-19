#pragma once
// A remote shell over the USB network (shell.py or `make shell` on the Mac)
#include <stdint.h>

#define SHELL_PORT 5002

// One command packet. Points *reply at the answer and returns its length
// (0 = not ours, ignore it). The answer may need several packets.
uint32_t shell_packet(const uint8_t *data, uint32_t len, const char **reply);
extern uint32_t shell_commands;
