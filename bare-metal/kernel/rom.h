#pragma once
// The boot ROM: the chip's own first code, burned in at the factory
void rom_page(void);
const char *rom_save(void);
