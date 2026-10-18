// Who's at EL3, above us, and what it did before starting us.
//
// We can't read EL3's memory or registers (that's the point of it), so we
// ask: `smc` ("secure monitor call") traps up to EL3 with a function number
// in x0, and EL3 answers in x0-x3. PSCI_VERSION works with any firmware;
// SMC_HELLO only means something to our own monitor (bare-metal/monitor),
// and ARM Trusted Firmware answers it with "unknown function".

#include <stdint.h>
#include "console.h"
#include "exception.h"
#include "monitor.h"

#define PSCI_VERSION 0x84000000
#define SMC_HELLO    0xc200ff00
#define SMC_REPORT   0xc200ff01
#define HELLO_MAGIC  0x4d4f4e49544f52UL   // "MONITOR"

// The report's entries, in the monitor's order (monitor/main.c)
enum {
	R_STEPS, R_X0, R_X1, R_KERNEL, R_FROM_LIST, R_TICKS_IN, R_TICKS_OUT,
	R_CNTFRQ_WAS, R_ECTLR_WAS, R_STIMER_WAS, R_SGRF_WAS, R_FIREWALL_WAS,
	R_GIC_LINES, R_CORES, R_CRASH_SLOT, R_CRASH_ESR, R_CRASH_ELR,
	R_CRASH_FAR, R_SMCS, R_LAST_SMC, R_COUNT
};

static uint64_t smc(uint64_t fn, uint64_t arg, uint64_t *second)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = arg;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	// ATF may change x4-x17 too (older calling rules), so tell the compiler
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			 "x13", "x14", "x15", "x16", "x17", "memory");
	if (second)
		*second = x1;
	return x0;
}

int monitor_ours(void)
{
	return smc(SMC_HELLO, 0, 0) == HELLO_MAGIC;
}

static void label(const char *name)
{
	console_color(GREY);
	uint32_t n = 0;
	for (; name[n]; n++)
		kputc(name[n]);
	for (; n < 9; n++)
		kputc(' ');
	console_color(WHITE);
}

static void psci_line(void)
{
	uint64_t v = smc(PSCI_VERSION, 0, 0);
	label("PSCI");
	kprintf("version %lu.%lu\n", v >> 16 & 0xffff, v & 0xffff);
}

// Try to read the monitor's first word, at 0x10000. The memory firewall
// should refuse: the read comes back as an error (a data abort), which the
// exception handler counts and skips, instead of crashing.
void monitor_peek(uint32_t row)
{
	uint64_t faults = fault_count;
	register uint64_t value __asm__("x0") = 0x5555;
	survive_faults = 1;
	__asm__ volatile("ldr %w0, [%1]" : "+r"(value) : "r"(0x10000UL) : "memory");
	survive_faults = 0;
	console_goto(0, row);
	label("Peek");
	if (fault_count != faults)
		kprintf("0x10000: refused, ESR %08lx\n", fault_esr);
	else
		kprintf("0x10000 = %08lx (not blocked!)\n", value);
}

void monitor_page(void)
{
	uint64_t count;
	console_init(WHITE, BG);
	console_color(YELLOW);
	if (smc(SMC_HELLO, 0, &count) != HELLO_MAGIC) {
		kputs("Someone else's EL3\n");
		label("Who");
		kputs("ARM Trusted Firmware, most\n");
		label("");
		kputs("likely (it doesn't know our\n");
		label("");
		kputs("hello call)\n");
		psci_line();
		console_goto(0, 22);
		console_color(YELLOW);
		kputs("A: button test");
		return;
	}

	uint64_t r[R_COUNT] = { 0 };
	for (uint32_t i = 0; i < R_COUNT && i < count; i++)
		r[i] = smc(SMC_REPORT, i, 0);

	kputs("Our own EL3 monitor!\n");
	label("Steps");
	kprintf("%lu of 6 done\n", r[R_STEPS]);
	label("Kernel");
	kprintf("%lx, %s\n", r[R_KERNEL], r[R_FROM_LIST] ? "the loader said" : "guessed");
	label("Given");
	kprintf("x0 %lx x1 %lx\n", r[R_X0], r[R_X1]);
	label("Took");
	kprintf("%lu us, from %lu ms\n", (r[R_TICKS_OUT] - r[R_TICKS_IN]) / 24,
		r[R_TICKS_IN] / 24000);
	label("SMP bit");
	kprintf("was %lu, now 1\n", r[R_ECTLR_WAS] >> 6 & 1);
	label("CNTFRQ");
	kprintf("was %lu\n", r[R_CNTFRQ_WAS]);
	label("Counter");
	kputs(r[R_STIMER_WAS] & 1 ? "was running already\n" : "was off: we started it\n");
	uint64_t s = r[R_SGRF_WAS];
	label("Secure");
	kprintf("%04lx %04lx %04lx %04lx\n", s & 0xffff, s >> 16 & 0xffff,
		s >> 32 & 0xffff, s >> 48 & 0xffff);
	label("");
	kputs("devices were; now none\n");
	label("Firewall");
	kprintf("was %02lx, now 0-1 MB ours\n", r[R_FIREWALL_WAS] & 0xff);
	label("GIC");
	kprintf("%lu IRQs, all non-secure\n", r[R_GIC_LINES]);
	label("Cores");
	kprintf("1-3 %s (PMU %04lx)\n", (r[R_CORES] & 0xe) == 0xe ? "off" : "ON",
		r[R_CORES] & 0xffff);
	psci_line();
	label("Crash");
	if (r[R_CRASH_SLOT] == ~0UL)
		kputs("none\n");
	else
		kprintf("slot %lu ESR %08lx\n%9s%lx %lx\n", r[R_CRASH_SLOT], r[R_CRASH_ESR],
			"", r[R_CRASH_ELR], r[R_CRASH_FAR]);
	label("SMCs");
	kprintf("%lu, this page's too\n", smc(SMC_REPORT, R_SMCS, 0));
	console_goto(0, 22);
	console_color(YELLOW);
	kputs("Y: peek  R: SPL page  A: buttons");
}

// ---- our SPL's report ---------------------------------------------------
// The SPL can't call us (it's gone by the time we run), so it leaves a
// report in memory at a fixed address, marked with a magic number.

#include "../spl/report.h"

const struct spl_report *spl_report(void)
{
	const struct spl_report *r = (const struct spl_report *)SPL_REPORT;
	return r->magic == SPL_MAGIC ? r : 0;
}

void spl_page(void)
{
	const struct spl_report *r = spl_report();
	console_init(WHITE, BG);
	console_color(YELLOW);
	if (!r) {
		kputs("Rockchip's loader started us\n");
		label("SPL");
		kputs("not ours: no report found\n");
	} else {
		const struct sd_info *s = &r->sd;
		kputs("Our own SPL!\n");
		label("Level");
		kprintf("EL%u, from the boot ROM\n", r->el);
		label("Started");
		kprintf("%lu ms after power-on\n", r->ticks_in / 24000);
		label("Card");
		kprintf("ready in %lu ms\n", (r->ticks_card - r->ticks_in) / 24000);
		// CID: maker ID (byte 15), name (5 letters, bytes 12-8)
		char name[6];
		for (int i = 0; i < 5; i++)
			name[i] = s->cid[(12 - i) / 4] >> ((12 - i) % 4 * 8) & 0xff;
		name[5] = 0;
		label("");
		kprintf("maker %02x \"%s\"\n", s->cid[3] >> 24, name);
		// CSD version 2 (SDHC/SDXC): size = (C_SIZE + 1) x 512 KB, with
		// C_SIZE in bits 48-69 of the 128 (csd[3] holds bits 96-127)
		uint32_t c_size = (s->csd[2] & 0x3f) << 16 | s->csd[1] >> 16;
		label("");
		if (s->csd[3] >> 30 == 1)
			kprintf("%lu MB, %s\n", ((uint64_t)c_size + 1) / 2, s->high_capacity ? "SDHC/XC" : "SDSC");
		else
			kprintf("%s, old CSD format\n", s->high_capacity ? "SDHC/XC" : "SDSC");
		label("Clock");
		kprintf("was %08x %08x\n", s->clksel16_was, s->clksel17_was);
		label("");
		kprintf("div %u; now 24 MHz /2 /1\n", s->clkdiv_was);
		label("Read");
		uint64_t us = (r->ticks_out - r->ticks_card) / 24;
		kprintf("%u KB in %lu ms", s->sectors_read / 2, us / 1000);
		if (us)
			kprintf(", %lu KB/s", (uint64_t)s->sectors_read * 512 * 1000 / 1024 * 1000 / us);
		kputc('\n');
		label("Kernel");
		kprintf("%x, %u bytes, CRC %s\n", r->kernel_addr, r->kernel_size, r->kernel_crc_ok ? "ok" : "BAD");
		label("Monitor");
		kprintf("%x, %u bytes\n", r->monitor_addr, r->monitor_size);
		label("Clocks");
		kprintf("CPU PLL %s, GPLL %s\n", (r->cru_mode & 3) == 1 ? "on" : "slow",
			(r->pmucru_mode & 3) == 1 ? "on" : "slow");
		label("");
		kprintf("mode %04x %04x sel0 %04x\n", r->cru_mode & 0xffff, r->pmucru_mode & 0xffff,
			r->clksel0 & 0xffff);
	}
	console_goto(0, 22);
	console_color(YELLOW);
	kputs("L: EL3 page  A: buttons");
}
