// A remote shell: commands typed on the Mac (shell.py, or `make shell`),
// run here, the answer sent back as text.
//
// Each command is one UDP packet to port 5002: a 4-byte sequence number,
// then the line. The answer is one or more packets back: the same
// sequence number, a part number (2 bytes), a "last part" flag (1 byte),
// then text. If an answer gets lost the Mac sends the same command again,
// with the same number; we recognise it and send the saved answer rather
// than running it twice (a poke, say).
//
// Commands run right here, inside the USB interrupt: the rest of the
// kernel waits until they're done, which keeps everything simple.

#include <stdarg.h>
#include <stdint.h>
#include "console.h"
#include "gpio.h"
#include "cpufreq.h"
#include "cores.h"
#include "matmul.h"
#include "rk817.h"
#include "thermal.h"
#include "dump.h"
#include "exception.h"
#include "log.h"
#include "shell.h"
#include "sysreg.h"
#include "timer.h"

#define OUT_SIZE (32 * 1024)

static char out[OUT_SIZE];
static uint32_t out_len, last_seq = ~0U;
static int have_last;
uint32_t shell_commands;

// The last 64 bytes are kept for saying the answer was cut short
static void put(char c)
{
	if (out_len < OUT_SIZE - 64)
		out[out_len++] = c;
}

static void say(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	kformat(put, fmt, args);
	va_end(args);
}

// ---- reading the command line -----------------------------------------------

#define MAX_WORDS 8
static char line[256];
static char *word[MAX_WORDS];
static uint32_t words;

static void split(const uint8_t *text, uint32_t len)
{
	if (len >= sizeof line)
		len = sizeof line - 1;
	for (uint32_t i = 0; i < len; i++)
		line[i] = (char)text[i];
	line[len] = 0;
	words = 0;
	for (char *p = line; *p && words < MAX_WORDS;) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			*p++ = 0;
		if (!*p)
			break;
		word[words++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			p++;
	}
}

static int equal(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

// A number: 0x... in hex, otherwise decimal. 0 if it isn't one.
static int number(const char *s, uint64_t *v)
{
	uint64_t n = 0, base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2, base = 16;
	if (!*s)
		return 0;
	for (; *s; s++) {
		uint32_t d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else if (*s == '_')   // 0xff2b_0000, for readability
			continue;
		else
			return 0;
		n = n * base + d;
	}
	*v = n;
	return 1;
}

// Word i as a number, or `otherwise` if it isn't there
static int arg(uint32_t i, uint64_t otherwise, uint64_t *v)
{
	if (i >= words) {
		*v = otherwise;
		return 1;
	}
	if (number(word[i], v))
		return 1;
	say("not a number: %s\n", word[i]);
	return 0;
}

// ---- touching memory without crashing ------------------------------------------
//
// A bad address (nothing mapped there) is a data abort. With
// survive_faults set, exception.c skips the instruction instead of showing
// the crash screen, and counts it: so we can tell. The loads and stores are
// in assembly with fixed registers so we know exactly which instruction
// gets skipped, and what's left in the register when it is.

static int peek32(uintptr_t a, uint32_t *v)
{
	uint64_t faults = fault_count;
	register uint32_t x __asm__("w0") = 0xdeadbeef;
	survive_faults = 1;
	__asm__ volatile("ldr %w0, [%1]" : "+r"(x) : "r"(a) : "memory");
	survive_faults = 0;
	*v = x;
	return fault_count == faults;
}

static int poke32(uintptr_t a, uint32_t v)
{
	uint64_t faults = fault_count;
	survive_faults = 1;
	__asm__ volatile("str %w0, [%1]\ndsb sy" : : "r"(v), "r"(a) : "memory");
	survive_faults = 0;
	return fault_count == faults;
}

static int check_aligned(uint64_t a)
{
	if (a & 3) {
		say("%lx: not a multiple of 4 (registers are read 32 bits at a time)\n", a);
		return 0;
	}
	return 1;
}

static void fault_said(uint64_t a)
{
	say("%08lx: fault (nothing there?) esr %lx\n", a, fault_esr);
}

// ---- the commands ---------------------------------------------------------------

static void c_help(void);

// peek ADDR [N]: N 32-bit words, 4 to a line
static void c_peek(void)
{
	uint64_t a, n;
	if (words < 2 || !arg(1, 0, &a) || !arg(2, 1, &n) || !check_aligned(a))
		return say("peek ADDR [N]\n");
	if (n > 1024)
		n = 1024;
	for (uint64_t i = 0; i < n; i++) {
		uint32_t v;
		if (!peek32(a + i * 4, &v)) {
			if (i % 4)
				say("\n");
			return fault_said(a + i * 4);
		}
		if (i % 4 == 0)
			say("%08lx:", a + i * 4);
		say(" %08x", v);
		if (i % 4 == 3 || i == n - 1)
			say("\n");
	}
}

// poke ADDR VALUE: write, then read back (a register may read differently:
// some bits are read-only, some clear themselves)
static void c_poke(void)
{
	uint64_t a, v;
	if (words != 3 || !arg(1, 0, &a) || !arg(2, 0, &v) || !check_aligned(a))
		return say("poke ADDR VALUE\n");
	uint32_t was, now;
	if (!peek32(a, &was))
		return fault_said(a);
	if (!poke32(a, (uint32_t)v))
		return fault_said(a);
	peek32(a, &now);
	say("%08lx: %08x -> %08x (reads %08x)\n", a, was, (uint32_t)v, now);
	klog("shell: poke %lx %x", a, (uint32_t)v);
}

// dump ADDR [BYTES]: hex and text, 16 bytes a line
static void c_dump(void)
{
	uint64_t a, n;
	if (words < 2 || !arg(1, 0, &a) || !arg(2, 256, &n) || !check_aligned(a))
		return say("dump ADDR [BYTES]\n");
	if (n > 4096)
		n = 4096;
	for (uint64_t at = 0; at < n; at += 16) {
		uint32_t w[4];
		for (int i = 0; i < 4; i++)
			if (!peek32(a + at + i * 4, &w[i]))
				return fault_said(a + at + i * 4);
		say("%08lx:", a + at);
		for (int i = 0; i < 16; i++)
			say("%s%02x", i == 8 ? "  " : " ", w[i / 4] >> (i % 4 * 8) & 0xff);
		say("  ");
		for (int i = 0; i < 16; i++) {
			uint8_t c = w[i / 4] >> (i % 4 * 8) & 0xff;
			put(c >= 32 && c < 127 ? (char)c : '.');
		}
		say("\n");
	}
}

// The CPU's own registers that say how it's set up
static void c_cpu(void)
{
	uint64_t midr = READ_SYSREG(midr_el1);
	say("CurrentEL   EL%u\n", current_el());
	say("MIDR        %08lx  (%s r%lup%lu)\n", midr,
	    (midr >> 4 & 0xfff) == 0xd04 ? "Cortex-A35" : "?", midr >> 20 & 15, midr & 15);
	say("MPIDR       %08lx  (core %lu)\n", READ_SYSREG(mpidr_el1), READ_SYSREG(mpidr_el1) & 0xff);
	uint64_t sctlr = READ_SYSREG(sctlr_el2);
	say("SCTLR_EL2   %08lx  (MMU %s, D-cache %s, I-cache %s)\n", sctlr,
	    sctlr & 1 ? "on" : "off", sctlr & 4 ? "on" : "off", sctlr & 4096 ? "on" : "off");
	say("HCR_EL2     %08lx\n", READ_SYSREG(hcr_el2));
	say("VBAR_EL2    %08lx\n", READ_SYSREG(vbar_el2));
	say("TTBR0_EL2   %08lx\n", READ_SYSREG(ttbr0_el2));
	say("DAIF        %lx\n", READ_SYSREG(daif) >> 6);
	say("CNTFRQ      %lu Hz\n", READ_SYSREG(cntfrq_el0));
	// What the CPU can do: each 4-bit field is a feature's level (0 = none)
	uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1), isar0 = READ_SYSREG(id_aa64isar0_el1);
	say("PFR0        %08lx  (FP %s, SIMD %s)\n", pfr0,
	    (pfr0 >> 16 & 15) == 15 ? "no" : (pfr0 >> 16 & 15) ? "+fp16" : "yes",
	    (pfr0 >> 20 & 15) == 15 ? "no" : (pfr0 >> 20 & 15) ? "+fp16" : "yes");
	say("ISAR0       %08lx  (AES %s, SHA1 %s, SHA2 %s, CRC32 %s, atomics %s, dot %s)\n", isar0,
	    (isar0 >> 4 & 15) >= 2 ? "+pmull" : (isar0 >> 4 & 15) ? "yes" : "no",
	    isar0 >> 8 & 15 ? "yes" : "no", isar0 >> 12 & 15 ? "yes" : "no",
	    isar0 >> 16 & 15 ? "yes" : "no", isar0 >> 20 & 15 ? "yes" : "no",
	    isar0 >> 44 & 15 ? "yes" : "no");
	uint64_t clidr = READ_SYSREG(clidr_el1);
	say("CLIDR       %08lx\n", clidr);
	// Each cache's shape: CSSELR picks one, CCSIDR describes it
	for (uint32_t level = 0; level < 7; level++) {
		uint32_t type = clidr >> (level * 3) & 7;
		if (!type)
			break;
		for (uint32_t instr = 0; instr < 2; instr++) {
			if (instr ? !(type & 1) : type < 2)
				continue;
			__asm__ volatile("msr csselr_el1, %0\nisb" : : "r"((uint64_t)(level << 1 | instr)));
			uint64_t c = READ_SYSREG(ccsidr_el1);
			uint64_t line_size = 16U << (c & 7), ways = (c >> 3 & 0x3ff) + 1, sets = (c >> 13 & 0x7fff) + 1;
			say("L%u %s     %lu KB: %lu sets x %lu ways x %lu bytes\n", level + 1,
			    instr ? "instr" : (type == 4 ? "unif " : "data "),
			    sets * ways * line_size / 1024, sets, ways, line_size);
		}
	}
	__asm__ volatile("msr csselr_el1, xzr\nisb");
}

// The PLLs: the clock generators everything else is divided from
static void c_clocks(void)
{
	say("APLL (CPU)    %u kHz\n", apll_khz());
	say("DPLL (RAM x2) %u kHz\n", dpll_khz());
	say("CPLL          %u kHz\n", cpll_khz());
	say("NPLL          %u kHz\n", npll_khz());
	say("GPLL          %u kHz\n", gpll_khz());
}

static void c_uptime(void)
{
	uint64_t ms = READ_SYSREG(cntpct_el0) / 24000;
	say("%lu.%03lu s since power-on; %lu timer interrupts\n", ms / 1000, ms % 1000, timer_ticks);
}

// How many times each interrupt has come, since start-up
static void c_irqs(void)
{
	for (uint32_t i = 0; i < MAX_IRQ; i++)
		if (irq_counts[i])
			say("%3u  %lu  %s\n", i, irq_counts[i], i == 30 ? "timer" : i == 94 ? "USB" :
			    i >= (uint32_t)GPIO_IRQ(0) && i <= (uint32_t)GPIO_IRQ(3) ? "GPIO (buttons)" : "");
	say("faults survived: %lu\n", fault_count);
}

// smc FN [X1 X2 X3]: call the EL3 monitor, show what it answers
static void c_smc(void)
{
	uint64_t fn, a1, a2, a3;
	if (words < 2 || !arg(1, 0, &fn) || !arg(2, 0, &a1) || !arg(3, 0, &a2) || !arg(4, 0, &a3))
		return say("smc FN [X1 X2 X3]   (0x84000000 is PSCI_VERSION)\n");
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			 "x13", "x14", "x15", "x16", "x17", "memory");
	say("x0 %lx  x1 %lx  x2 %lx  x3 %lx\n", x0, x1, x2, x3);
}

// mhz [MHZ]: show, or set, the CPU's speed (and its voltage with it)
static void c_mhz(void)
{
	uint64_t want;
	if (words > 1) {
		if (!arg(1, 0, &want))
			return;
		uint32_t was = cpu_mhz();
		const char *err = cpu_set_mhz((uint32_t)want);
		if (err) {
			say("%s. Speeds:\n", err);
			cpu_speeds(say);
			return;
		}
		klog("cpu: %u -> %u MHz", was, cpu_mhz());
	}
	say("CPU %u MHz (PLL), %u measured, vdd_arm %u mV\n", cpu_mhz(), cpu_measure_mhz(), cpu_mv());
}

// stress [MS]: the same work again and again (mixing numbers, writing and
// reading 256 KB of RAM), for MS milliseconds; every result must match the
// first. An under-volted CPU gets the odd wrong answer rather than crashing,
// so this is how to tell. (Up to 2 s: the Mac stops waiting for an answer
// after 3.)
static uint32_t stress_buf[64 * 1024];
static void c_stress(void)
{
	uint64_t ms;
	if (!arg(1, 1000, &ms))
		return;
	if (ms > 2000)
		ms = 2000;
	uint64_t end = READ_SYSREG(cntpct_el0) + ms * 24000, first = 0, rounds = 0, wrong = 0;
	while (READ_SYSREG(cntpct_el0) < end) {
		uint64_t h = 0x9e3779b97f4a7c15ULL;
		for (uint32_t i = 0; i < 64 * 1024; i++) {
			h ^= h << 13, h ^= h >> 7, h ^= h << 17;   // xorshift: lots of ALU work
			stress_buf[i] = (uint32_t)h;
		}
		for (uint32_t i = 0; i < 64 * 1024; i++)
			h = h * 31 + stress_buf[i];
		if (!rounds++)
			first = h;
		else if (h != first)
			wrong++;
	}
	say("%lu rounds in %lu ms at %u MHz: %lu wrong\n", rounds, ms, cpu_mhz(), wrong);
}

static void c_temp(void)
{
	static const char *const names[2] = { "CPU", "GPU" };
	for (uint32_t ch = 0; ch < 2; ch++) {
		int32_t t;
		uint32_t code = *(volatile uint32_t *)(0xff280020UL + 4 * ch) & 0xfff;
		if (thermal_read(ch, &t))
			say("%s  no reading (code %u)\n", names[ch], code);
		else
			say("%s  %d.%d C  (code %u)\n", names[ch], t / 10, t % 10, code);
	}
}

static void c_battery(void)
{
	struct battery b;
	if (rk817_battery(&b))
		return say("the power chip didn't answer\n");
	say("%u%%  %u mV  %d mA %s  %s\n", rk817_battery_percent(&b), b.mv, b.ma,
	    b.ma < 0 ? "out" : "in", b.plugged_in ? rk817_charge_status(b.status) : "not plugged in");
}

// cores: start cores 1-3, and have each say who it is and how fast it runs
static struct who { uint64_t mpidr, el, mhz; } who[4];
static void who_job(void *p)
{
	struct who *w = p;
	w->mpidr = READ_SYSREG(mpidr_el1);
	w->el = current_el();
	w->mhz = cpu_measure_mhz();
}
static void c_cores(void)
{
	const char *err = cores_start();
	if (err)
		return say("%s\n", err);
	uint64_t t[4] = {0};
	who_job(&who[0]);
	for (uint32_t c = 1; c < 4; c++) {
		uint64_t t0 = READ_SYSREG(cntpct_el0);
		core_run(c, who_job, &who[c]);
		core_wait(c);
		t[c] = READ_SYSREG(cntpct_el0) - t0;
	}
	for (uint32_t c = 0; c < 4; c++) {
		say("core %u: MPIDR %08lx, EL%lu, %lu MHz measured", c, who[c].mpidr, who[c].el, who[c].mhz);
		if (c)
			say(", %lu jobs, this one %lu us there and back", core_jobs(c), t[c] / 24);
		say(c ? "\n" : " (interrupts on: the one doing this)\n");
	}
}

static void c_matmul(void)
{
	uint64_t n;
	if (arg(1, 128, &n))
		matmul_bench((uint32_t)n, say);
}

static void c_say(void)
{
	for (uint32_t i = 1; i < words; i++)
		say("%s%s", word[i], i + 1 < words ? " " : "");
	say("\n");
}

static const struct command {
	const char *name, *args, *what;
	void (*run)(void);
} commands[] = {
	{ "help",   "",               "this list", c_help },
	{ "peek",   "ADDR [N]",       "read N 32-bit words", c_peek },
	{ "poke",   "ADDR VALUE",     "write a 32-bit word, and read it back", c_poke },
	{ "dump",   "ADDR [BYTES]",   "memory as hex and text", c_dump },
	{ "cpu",    "",               "the CPU's own set-up registers, caches", c_cpu },
	{ "clocks", "",               "the PLLs", c_clocks },
	{ "uptime", "",               "time since power-on", c_uptime },
	{ "irqs",   "",               "interrupt counts", c_irqs },
	{ "mhz",    "[MHZ]",          "the CPU's speed: show, or set", c_mhz },
	{ "stress", "[MS]",           "check the CPU gets the same answers", c_stress },
	{ "temp",   "",               "the chip's temperature", c_temp },
	{ "battery", "",              "charge, voltage, current", c_battery },
	{ "matmul", "[N]",            "time a matrix multiply, plain and NEON", c_matmul },
	{ "cores",  "",               "start cores 1-3, and hear from each", c_cores },
	{ "smc",    "FN [X1 X2 X3]",  "call the EL3 monitor", c_smc },
	{ "echo",   "WORDS",          "say them back", c_say },
};
#define COMMANDS (sizeof commands / sizeof commands[0])

static void c_help(void)
{
	for (uint32_t i = 0; i < COMMANDS; i++) {
		uint32_t n = 0;
		say("%s %s", commands[i].name, commands[i].args);
		for (const char *p = commands[i].name; *p; p++)
			n++;
		for (const char *p = commands[i].args; *p; p++)
			n++;
		for (; n < 22; n++)
			put(' ');
		say("%s\n", commands[i].what);
	}
	say("Numbers: 0x... is hex, otherwise decimal. Bad addresses are survived.\n");
}

// ---- one packet from the Mac ---------------------------------------------------

uint32_t shell_packet(const uint8_t *data, uint32_t len, const char **reply)
{
	if (len < 4)
		return 0;
	uint32_t seq = data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24;
	if (!have_last || seq != last_seq) {
		out_len = 0;
		split(data + 4, len - 4);
		if (words) {
			uint32_t i = 0;
			while (i < COMMANDS && !equal(commands[i].name, word[0]))
				i++;
			if (i < COMMANDS)
				commands[i].run();
			else
				say("%s: no such command (try help)\n", word[0]);
		}
		if (out_len == OUT_SIZE - 64)
			for (const char *p = "\n...(answer too long, cut short)\n"; *p; p++)
				out[out_len++] = *p;
		last_seq = seq, have_last = 1;
		shell_commands++;
	}
	*reply = out;
	return out_len;
}
