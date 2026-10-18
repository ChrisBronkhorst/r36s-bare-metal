#pragma once
// What Rockchip's RAM setup did (ram.c)
void ram_page(void);
const char *ram_save(void);   // 0 when saved to the card, else what failed
void ram_card_info(void);     // after a failed save: the card driver's state
