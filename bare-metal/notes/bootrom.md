# The PX30 boot ROM, read from our dump

Dump: `kernel/dumps/bootrom-20261019-041258.{bin,s}`. 32 KB at 0xffff0000,
mirrored to fill 64 KB. AArch64. Non-secure reads are refused; our EL3
monitor read it (SMC 0xc200ff02).

## Reset (+0x0)
- Cores 1-3: write 1 to SRAM 0xff0e0004, then `wfe` until that word is
  0xdeadbeaf, then jump to the address at 0xff0e0008.
- Core 0: zero x0-x30, sp = 0xff0e1000, VBAR_EL3 = 0xffff5800 (vectors are
  just `b .`), main at +0x1ca4.

## Boot devices (table at +0x5be8, 5 x 32 bytes)
Each: init, read(sector, buf, bytes), find_header(copy, buf, &sector),
copies to try, id. Tried in order; the first that loads and runs wins:
1. NAND (controller 0xff3b0000), 50 copies
2. eMMC (probably; shares SD's functions), 5
3. SPI NOR (probably), 10
4. SPI NAND (probably), 10
5. SD card, 5
Then USB "maskrom" (+0x8a8, USB OTG at 0xff300000).

For SD (and eMMC) copy n's header is at sector 64 | n << 10:
64, 1088, 2112, 3136, 4160.

## Loading (+0x60c)
1. Read the 512-byte header into SRAM 0xff0e1000. Its first word must be
   0xfcdc8c3b, which is 0x0ff0aa55 after RC4.
2. RC4-decrypt it (+0x288c). The ROM stores the key's ready-made 256-byte
   key schedule (+0x5cac), not the 16-byte key itself.
3. Header fields: +0x8 flags (bit 0: data not RC4-encrypted), +0xc the data
   offset in sectors, +0x1fa init_size, +0x1fc init_boot_size (both in
   sectors). init_size must be a multiple of 4 and 4-24 sectors (2-12 KB:
   SRAM 0xff0e1000-0xff0e4000); init_boot_size < 5 means 10.
4. Read in 2 KB chunks (4 sectors), each RC4-decrypted per 512 bytes unless
   flag bit 0. A first chunk starting "RSAK" (a key block) is skipped.
5. Once init_size is in: the first word must be "RK33". Call 0xff0e1004;
   non-zero = failed, try the next copy.
6. Then the rest (init_boot_size - init_size) goes to DRAM at 0, and the
   ROM jumps there (`br x0`, x0 = 0).

## Secure boot
0xff0e000c says whether it's on (off here). If it were, every chunk would
be SHA-256 hashed (+0x3d30) and checked against an RSA signature (+0x3c28,
crypto block at 0xff0b0000). Next to "RK33" is SHA-256 of empty input,
likely "no key hash burned in".

## The SD card path (entry 5: init +0x32e0, read +0x33b0, header +0x344c)
Controller 0 = SD card at 0xff370000, 1 = eMMC at 0xff390000 (same code).
Delays (+0x5dc) are busy loops in microseconds, calibrated by the CPU MHz
kept at SRAM 0xff0e0000.

Controller setup (+0x2cd0):
1. Pins to the SD function: GRF 0xff140018 = 0xff001100, 0xff14001c =
   0xffff1111 (GPIO1 iomux), plus a PMUGRF write.
2. Clock: CRU clksel16 = 0xc0ffc01f: the 24 MHz crystal / 32, and the
   controller halves it: 375 kHz, inside the 100-400 kHz the spec wants.
3. CTRL = 3 (reset controller and FIFO), CLKDIV 0, CLKENA 1, then the
   "update clock" command (0x80202000), polled until accepted.
4. TMOUT 0xffffff40, FIFOTH 0x307f0080, clear RINTSTS.

Card start-up (+0x3304, +0x314c):
1. CMD0 with 80 start-up clocks (CMD register 0x8100), 100 us wait.
2. CMD8 arg 0x1aa. No answer: CMD55. No answer to that either: MMC path
   (+0x1af8); answered: old SD v1 card (ACMD41 without HCS).
3. ACMD41 (0x40ff8000) up to 2040 times until ready; bit 30 = SDHC, so
   sector numbers instead of byte addresses (flag at 0xff0e0204).
4. CMD2 (CID), CMD3 (RCA, kept at 0xff0e0200).
5. Faster clock (+0x30bc): clksel16 = 0x000f: GPLL / 16, halved by the
   controller. GPLL is 768 MHz (set at +0x42c4: con0 0x30c0, con1 0x42), so
   the card runs at 24 MHz.
6. CMD7 (select), CMD16 512, ACMD42 (DAT3 pull-up off), ACMD6 4-bit bus +
   CTYPE = 1.
No CMD9: the ROM never asks the card's size.

Reads (+0x33b0, +0x2e08): CMD17 for the 512-byte header, CMD18 + CMD12 for
each 2 KB chunk. Data comes through the FIFO, polled: no DMA, no interrupts.
