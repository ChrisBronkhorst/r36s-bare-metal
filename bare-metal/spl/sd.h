#pragma once
#include <stdint.h>

// What the driver found out about the card (also in the SPL's report)
struct sd_info {
	uint32_t step;          // how far sd_init got (6 = done)
	uint32_t last_cmd, last_status;
	uint32_t clksel16_was, clksel17_was, clkdiv_was;   // as the boot ROM left them
	uint32_t v2, high_capacity, ocr, rca;
	uint32_t cid[4], csd[4];
	uint32_t sectors_read;
};
extern struct sd_info sd;

int sd_init(void);   // 0 when the card is ready to read
int sd_read(uint32_t sector, uint32_t count, void *dst);   // 0 on success
