// Our SPL ("secondary program loader"), in place of Rockchip's loader.
//
// Power-on, so far:
//   1. The boot ROM, inside the chip, reads the card's sector 64: a header
//      saying where two programs are.
//   2. It loads the first into the chip's 10 KB of SRAM and calls it:
//      Rockchip's RAM setup, which brings the 1 GB of DDR memory to life
//      and returns.
//   3. It loads the second, us, into that RAM at address 0, and calls us.
// We're at EL3, with no MMU and no data cache. Our job: read the monitor
// and the kernel off the card, where they've always been, and start the
// monitor, telling it where the kernel is. All with our own SD driver.
//
// If something fails, the backlight flashes the step number, over and
// over (the screen is still dark: setting that up is the kernel's job).

#include <stdint.h>
#include "backlight.h"
#include "board.h"
#include "report.h"
#include "sd.h"

#define LOADER_SECTOR 16384   // uboot.img: our kernel, in Rockchip's LOADER format
#define TRUST_SECTOR  24576   // trust.img: our monitor, as "BL31"

static struct spl_report *const rep = (struct spl_report *)SPL_REPORT;

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

static void delay_ms(uint32_t ms)
{
	uint64_t t = ticks();
	while (ticks() - t < ms * 24000ULL)
		;
}

// Failed at `step`: flash it on the backlight, forever. That needs the
// kernel's board set-up (I2C to the power chip, and the PWM), built in here.
static void __attribute__((noreturn)) fail(uint32_t step)
{
	rep->failed = step;
	rep->sd = sd;
	board_init();
	for (;;) {
		for (uint32_t i = 0; i < step; i++) {
			backlight_set(100);
			delay_ms(250);
			backlight_set(0);
			delay_ms(350);
		}
		delay_ms(2000);
	}
}

// A crash in here (start.S's vectors come here): 10 flashes
void spl_crash(uint64_t esr, uint64_t elr)
{
	rep->crash_esr = esr;
	rep->crash_elr = elr;
	fail(10);
}

// Rockchip's CRC-32 (pack-loader.py has the same): polynomial 0x04c10db7,
// most significant bit first, starting at 0
static uint32_t crc32_rk(const volatile uint8_t *p, uint32_t n)
{
	uint32_t crc = 0;
	while (n--) {
		crc ^= (uint32_t)*p++ << 24;
		for (int i = 0; i < 8; i++)
			crc = crc & 0x80000000 ? crc << 1 ^ 0x04c10db7 : crc << 1;
	}
	return crc;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t header[2048] __attribute__((aligned(16)));

// The kernel: a 2 KB "LOADER" header, then the program
static void load_kernel(void)
{
	if (sd_read(LOADER_SECTOR, 4, header))
		fail(6);
	static const char magic[8] = "LOADER  ";
	for (int i = 0; i < 8; i++)
		if (header[i] != magic[i])
			fail(6);
	uint32_t addr = le32(header + 0x10), size = le32(header + 0x14);
	rep->kernel_addr = addr;
	rep->kernel_size = size;
	// Only where it's meant to go, and not over us or the monitor
	if (addr != 0x200000 || size == 0 || size > 16 << 20)
		fail(6);
	if (sd_read(LOADER_SECTOR + 4, (size + 511) / 512, (void *)(uintptr_t)addr))
		fail(7);
	rep->kernel_crc = crc32_rk((const volatile uint8_t *)(uintptr_t)addr, size);
	rep->kernel_crc_ok = rep->kernel_crc == le32(header + 0x18);
	if (!rep->kernel_crc_ok)
		fail(7);
}

// The monitor: the trust image's first part, which must be "BL31".
// Returns where it starts.
static uintptr_t load_monitor(void)
{
	if (sd_read(TRUST_SECTOR, 4, header))
		fail(8);
	if (le32(header) != 0x58334c42)   // "BL3X"
		fail(8);
	uint32_t size_word = le32(header + 12);
	uint32_t table = (size_word & 0xffff) * 4 + 256;   // after the signature
	if ((size_word >> 16) < 1 || table + 16 > sizeof header)
		fail(8);
	if (le32(header + table) != 0x31334c42)   // "BL31"
		fail(8);
	uint32_t at = le32(header + table + 4), sectors = le32(header + table + 8);
	uint32_t addr = le32(header + 0x320 + 32);
	rep->monitor_addr = addr;
	rep->monitor_size = sectors * 512;
	extern char _end[];
	if (addr < (uintptr_t)_end || addr + sectors * 512 > 0x100000 || sectors == 0)
		fail(8);
	if (sd_read(TRUST_SECTOR + at, sectors, (void *)(uintptr_t)addr))
		fail(9);
	return addr;
}

// The list ATF expects from Rockchip's loader (our monitor reads it too):
// a header, then pointers; the 4th points to the kernel's entry point.
struct param_header { uint8_t type, version; uint16_t size; uint32_t attr; };
struct entry_point { struct param_header h; uint64_t pc; uint32_t spsr, pad; uint64_t args[8]; };
struct bl31_params {
	struct param_header h;
	uint64_t bl31_image, bl32_entry, bl32_image, bl33_entry, bl33_image;
};
static struct entry_point kernel_entry;
static struct bl31_params params;

void spl_main(uint64_t el)
{
	for (uint32_t i = 0; i < sizeof *rep / 8; i++)
		((volatile uint64_t *)rep)[i] = 0;
	rep->ticks_in = ticks();
	rep->magic = SPL_MAGIC;
	rep->el = el;
	rep->cru_mode = rd(0xff2b00a0);
	rep->pmucru_mode = rd(0xff2bc020);
	rep->clksel0 = rd(0xff2b0100);

	if (sd_init())
		fail(sd.step);
	rep->ticks_card = ticks();
	load_kernel();
	uintptr_t monitor = load_monitor();
	rep->sd = sd;

	kernel_entry.h = (struct param_header){ 1, 1, sizeof kernel_entry, 0 };   // PARAM_EP
	kernel_entry.pc = rep->kernel_addr;
	kernel_entry.spsr = 0x3c9;
	params.h = (struct param_header){ 3, 1, sizeof params, 0 };   // PARAM_BL31
	params.bl33_entry = (uintptr_t)&kernel_entry;
	rep->ticks_out = ticks();

	// The monitor arrived as data; make sure no stale instructions are
	// cached for those addresses, then jump in. It never comes back.
	__asm__ volatile("dsb sy\n ic iallu\n dsb sy\n isb" ::: "memory");
	((void (*)(uint64_t, uint64_t))monitor)((uintptr_t)&params, 0);
	fail(11);
}

// The compiler may call these for struct copies; there's no C library
void *memset(void *dst, int c, unsigned long n)
{
	volatile uint8_t *d = dst;
	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	volatile uint8_t *d = dst;
	const volatile uint8_t *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}
