// SD card driver: just enough to read sectors.
//
// Two layers:
//  - the controller, a Synopsys "DesignWare" MMC host at 0xff370000. We hand
//    it a command (number, argument, what kind of answer to expect), it
//    clocks it out on the CMD wire and collects the card's response. Data
//    arrives on separate wires (DAT0-3) and piles up in a 256-word FIFO,
//    which we empty by reading one register over and over.
//  - the card, which follows the SD protocol: after power-on it only
//    understands a few commands, at 400 kHz at most. We walk it through
//    "identification" to "transfer" state, where it reads sectors.
//
// The boot ROM read the card before us, so the pins, power and controller
// already work. We still reset the card and do it all ourselves.

#include "sd.h"

#define SD   0xff370000UL
#define CRU  0xff2b0000UL

// controller registers
#define CTRL    0x000
#define CLKDIV  0x008
#define CLKSRC  0x00c
#define CLKENA  0x010
#define TMOUT   0x014
#define CTYPE   0x018
#define BLKSIZ  0x01c
#define BYTCNT  0x020
#define INTMASK 0x024
#define CMDARG  0x028
#define CMD     0x02c
#define RESP0   0x030
#define RINTSTS 0x044
#define STATUS  0x048
#define FIFOTH  0x04c
#define DATA    0x200

// CMD register bits
#define C_RESP       (1U << 6)    // the card answers
#define C_LONG       (1U << 7)    // ...with 136 bits, not 48
#define C_CRC        (1U << 8)    // check the answer's CRC
#define C_DATA       (1U << 9)    // data follows, on the DAT wires
#define C_AUTO_STOP  (1U << 12)   // send CMD12 (stop) after the last block
#define C_WRITE      (1U << 10)   // data goes to the card, not from it
#define C_WAIT_PREV  (1U << 13)   // wait for the previous data to finish
#define C_INIT       (1U << 15)   // 80 clocks first: wakes the card up
#define C_CLOCK_ONLY (1U << 21)   // no command, just load the new clock setup
#define C_HOLD       (1U << 29)
#define C_START      (1U << 31)   // go; the controller clears it when taken

// RINTSTS bits ("raw interrupt status": we poll them, no interrupts)
#define I_RESP_ERR  (1U << 1)
#define I_CMD_DONE  (1U << 2)
#define I_DATA_OVER (1U << 3)
#define I_RESP_CRC  (1U << 6)
#define I_DATA_CRC  (1U << 7)
#define I_RESP_TO   (1U << 8)
#define I_DATA_TO   (1U << 9)
#define I_STARVE    (1U << 10)
#define I_START_ERR (1U << 13)
#define I_END_ERR   (1U << 15)
#define I_CMD_ERRS  (I_RESP_ERR | I_RESP_CRC | I_RESP_TO)
#define I_DATA_ERRS (I_DATA_CRC | I_DATA_TO | I_STARVE | I_START_ERR | I_END_ERR)

// what kind of answer each command gets (the SD spec's "response types")
#define R_NONE 0
#define R1     (C_RESP | C_CRC)            // 32 bits of card status
#define R2     (C_RESP | C_LONG | C_CRC)   // 128 bits: CID or CSD
#define R3     C_RESP                      // the OCR, sent without a CRC
#define R6     (C_RESP | C_CRC)            // the card's new address
#define R7     (C_RESP | C_CRC)            // echo of CMD8's check pattern

struct sd_info sd;

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	return t;
}
#define MS 24000ULL   // counter ticks in a millisecond

// Wait until (register & mask) == want, up to ms milliseconds
static int wait_for(uintptr_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t t = ticks();
	while ((rd(reg) & mask) != want)
		if (ticks() - t > ms * MS)
			return -1;
	return 0;
}

// The controller takes clock changes through the command register too
static int clock_update(void)
{
	wr(SD + CMD, C_START | C_CLOCK_ONLY | C_WAIT_PREV);
	return wait_for(SD + CMD, C_START, 0, 100);
}

// The card's clock: cclk_in / (2 x div), or cclk_in itself for div 0
static int set_clock(uint32_t div)
{
	wr(SD + CLKENA, 0);
	if (clock_update())
		return -1;
	wr(SD + CLKDIV, div);
	wr(SD + CLKSRC, 0);
	if (clock_update())
		return -1;
	wr(SD + CLKENA, 1);   // on, and kept running between commands
	return clock_update();
}

// Send one command. Returns 0, or the error bits; the answer is in resp.
static uint32_t command(uint32_t index, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
	if (wait_for(SD + STATUS, 1U << 9, 0, 500))   // card still busy
		return 0x10000;
	wr(SD + RINTSTS, 0xffffffff);   // clear old events (write 1 to clear)
	wr(SD + CMDARG, arg);
	wr(SD + CMD, C_START | C_HOLD | flags | index);
	if (wait_for(SD + RINTSTS, I_CMD_DONE, I_CMD_DONE, 500))
		return 0x20000;
	uint32_t errors = rd(SD + RINTSTS) & I_CMD_ERRS;
	sd.last_cmd = index;
	sd.last_status = rd(SD + RINTSTS);
	if (errors)
		return errors;
	if (resp)
		for (int i = 0; i < 4; i++)
			resp[i] = rd(SD + RESP0 + 4 * i);
	return 0;
}

// "Application" commands (ACMDn) are CMD55, then the command
static uint32_t app_command(uint32_t index, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
	uint32_t r[4], err = command(55, sd.rca << 16, R1, r);
	return err ? err : command(index, arg, flags, resp);
}

// Clocks: clk_sdmmc from the 24 MHz crystal, not divided. The controller
// gets half of that (a fixed /2 on Rockchip), so cclk_in = 12 MHz.
static void clock_source(void)
{
	sd.clksel16_was = rd(CRU + 0x100 + 16 * 4);
	sd.clksel17_was = rd(CRU + 0x100 + 17 * 4);
	sd.clkdiv_was = rd(SD + CLKDIV);
	wr(SD + CLKENA, 0);   // card clock off while its source changes
	clock_update();
	wr(CRU + 0x100 + 16 * 4, 0xc0ffU << 16 | 3U << 14 | 0);   // xin24m, /1
	wr(CRU + 0x100 + 17 * 4, 0x8000U << 16 | 0);              // the plain divider
	wr(CRU + 0x200 + 6 * 4, (1U << 12 | 1U << 13 | 1U << 15) << 16);   // gates open
}

int sd_init(void)
{
	uint32_t r[4], err;
	sd.step = 1;   // the controller
	clock_source();
	wr(SD + CTRL, 7);   // reset the controller, its FIFO and DMA
	if (wait_for(SD + CTRL, 7, 0, 100))
		return -1;
	wr(SD + INTMASK, 0);              // we poll, no interrupts
	wr(SD + RINTSTS, 0xffffffff);
	wr(SD + TMOUT, 0xffffff40);       // longest data timeout, 64-clock response timeout
	wr(SD + FIFOTH, 2U << 28 | 127U << 16 | 128);
	wr(SD + CTYPE, 0);                // 1 data wire for now
	if (set_clock(30))                // 12 MHz / 60 = 200 kHz: slow enough for anything
		return -1;

	sd.step = 2;   // reset the card, and ask what it is
	command(0, 0, R_NONE | C_INIT, 0);   // CMD0 GO_IDLE: no answer expected
	// CMD8 SEND_IF_COND: "I supply 2.7-3.6 V", plus a pattern to echo.
	// Cards older than SD 2.0 don't know it (and can't be over 2 GB).
	err = command(8, 0x1aa, R7, r);
	sd.v2 = !err && (r[0] & 0xfff) == 0x1aa;

	sd.step = 3;   // ACMD41: start up, which can take up to a second
	uint64_t t = ticks();
	do {
		// HCS (bit 30): we understand cards over 2 GB. Bits 15-23: 2.7-3.6 V.
		err = app_command(41, (sd.v2 ? 1U << 30 : 0) | 0xff8000, R3, r);
		if (err)
			return -1;
		if (ticks() - t > 1500 * MS)
			return -1;
	} while (!(r[0] & 1U << 31));   // bit 31: done powering up
	sd.ocr = r[0];
	sd.high_capacity = r[0] >> 30 & 1;   // SDHC/SDXC: addresses are sectors, not bytes

	sd.step = 4;   // identification: name, address, size
	if (command(2, 0, R2, sd.cid))        // CMD2: its CID (maker, name, serial)
		return -1;
	if (command(3, 0, R6, r))             // CMD3: "pick an address"
		return -1;
	sd.rca = r[0] >> 16;
	if (command(9, sd.rca << 16, R2, sd.csd))   // CMD9: its CSD (size, speeds)
		return -1;
	if (command(7, sd.rca << 16, R1, r))  // CMD7: select it: now in transfer state
		return -1;
	if (wait_for(SD + STATUS, 1U << 9, 0, 500))   // CMD7 can hold DAT0 busy
		return -1;

	sd.step = 5;   // full speed: 4 data wires, faster clock
	if (app_command(6, 2, R1, r))   // ACMD6: bus width 4
		return -1;
	wr(SD + CTYPE, 1);
	if (command(16, 512, R1, r))    // CMD16: 512-byte blocks (old cards need it)
		return -1;
	if (set_clock(0))               // the whole 12 MHz: under the 25 MHz limit
		return -1;
	sd.step = 6;
	return 0;
}

// Read `count` 512-byte sectors starting at `sector` to `dst`.
// CMD18 READ_MULTIPLE_BLOCK; the controller sends CMD12 to stop at the end.
int sd_read(uint32_t sector, uint32_t count, void *dst)
{
	uint32_t r[4];
	volatile uint32_t *out = dst;
	uint32_t words = count * 128;

	wr(SD + CTRL, rd(SD + CTRL) | 2);   // empty the FIFO first
	if (wait_for(SD + CTRL, 2, 0, 100))
		return -1;
	wr(SD + BLKSIZ, 512);
	wr(SD + BYTCNT, count * 512);
	uint32_t addr = sd.high_capacity ? sector : sector * 512;
	uint32_t err = count == 1 ? command(17, addr, R1 | C_DATA | C_WAIT_PREV, r)
				  : command(18, addr, R1 | C_DATA | C_WAIT_PREV | C_AUTO_STOP, r);
	if (err)
		return -1;
	// Empty the FIFO as it fills, until every word is in
	uint64_t t = ticks();
	while (words) {
		uint32_t status = rd(SD + RINTSTS);
		if (status & I_DATA_ERRS) {
			sd.last_status = status;
			return -1;
		}
		uint32_t n = rd(SD + STATUS) >> 17 & 0x1fff;   // words in the FIFO
		if (n > words)
			n = words;
		for (uint32_t i = 0; i < n; i++)
			*out++ = rd(SD + DATA);
		words -= n;
		if (!n && ticks() - t > 1000 * MS)
			return -1;
	}
	// ...and for the controller to say the transfer (and its CMD12) is over
	if (wait_for(SD + RINTSTS, I_DATA_OVER, I_DATA_OVER, 500))
		return -1;
	if (rd(SD + RINTSTS) & I_DATA_ERRS)
		return -1;
	sd.sectors_read += count;
	return 0;
}

// Write `count` 512-byte sectors from `src`, starting at `sector`.
// CMD25 WRITE_MULTIPLE_BLOCK: the mirror image of reading. We fill the FIFO
// as it empties; afterwards the card holds DAT0 low ("busy") while it
// programs its flash, and we wait for that too.
int sd_write(uint32_t sector, uint32_t count, const void *src)
{
	uint32_t r[4];
	const volatile uint32_t *in = src;
	uint32_t words = count * 128;

	wr(SD + CTRL, rd(SD + CTRL) | 2);   // empty the FIFO first
	if (wait_for(SD + CTRL, 2, 0, 100))
		return -1;
	wr(SD + BLKSIZ, 512);
	wr(SD + BYTCNT, count * 512);
	uint32_t addr = sd.high_capacity ? sector : sector * 512;
	uint32_t err = count == 1 ? command(24, addr, R1 | C_DATA | C_WRITE | C_WAIT_PREV, r)
				  : command(25, addr, R1 | C_DATA | C_WRITE | C_WAIT_PREV | C_AUTO_STOP, r);
	if (err)
		return -1;
	uint64_t t = ticks();
	while (words) {
		uint32_t status = rd(SD + RINTSTS);
		if (status & I_DATA_ERRS) {
			sd.last_status = status;
			return -1;
		}
		if (rd(SD + STATUS) & 1U << 3) {   // FIFO full: wait
			if (ticks() - t > 1000 * MS)
				return -1;
			continue;
		}
		wr(SD + DATA, *in++);
		words--;
	}
	if (wait_for(SD + RINTSTS, I_DATA_OVER, I_DATA_OVER, 2000))
		return -1;
	if (rd(SD + RINTSTS) & I_DATA_ERRS)
		return -1;
	if (wait_for(SD + STATUS, 1U << 9, 0, 2000))   // the card's busy signal
		return -1;
	return 0;
}
