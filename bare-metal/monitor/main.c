// Our EL3 monitor, in place of ARM Trusted Firmware (ATF).
//
// The RK3326 splits the machine in two: a secure world and a non-secure
// one. Every bus access carries a "secure" bit, and hardware along the way
// (the GIC, the peripheral bus, the memory controller's firewall) can refuse
// non-secure accesses. Out of reset, everything starts out secure. EL3
// decides what the non-secure world may use, then drops into it.
//
// Start-up, in the order ATF does it on this chip (plat/rockchip/px30 in
// ATF's source):
//   1. the CPU: join the cores' cache coherency, counter frequency
//   2. the counter behind the CPU's timer
//   3. peripherals and DMA: all non-secure
//   4. the memory firewall: our own megabyte stays secure
//   5. the GIC: every interrupt non-secure, so the kernel can use them
//   6. the CPU state the kernel starts in, at EL2
// Each step is noted in `report`, which the kernel can read with `smc`.
//
// After that we only run when the kernel executes `smc`: the "Power State
// Coordination Interface" (PSCI) calls Linux uses too, plus our own. One
// of them, CPU_ON, starts the other three cores: each comes through here
// too, for its own share of steps 1, 5 and 6.

#include <stdint.h>

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

#define SYSREG(name) ({ uint64_t v; __asm__ volatile("mrs %0, " #name : "=r"(v)); v; })
#define SET_SYSREG(name, v) __asm__ volatile("msr " #name ", %0" : : "r"((uint64_t)(v)))

#define CRU      0xff2b0000UL   // clocks and resets
#define PMUCRU   0xff2bc000UL   // the always-on part, with GPLL
#define PMU      0xff000000UL   // power domains
#define SGRF     0xff11c000UL   // secure settings: who may use which peripheral
#define STIMER   0xff220000UL   // secure timers: channel 1 drives the CPU's counter
#define FIREWALL 0xff534000UL   // the memory controller's firewall
#define GICD     0xff131000UL   // interrupt controller: distributor
#define GICC     0xff132000UL   //                       this core's interface
#define PMUSRAM  0xff020000UL   // 64 KB of always-on SRAM (secure only)
#define PMUSGRF  0xff050000UL   // the PMU's secure settings
#define GRF      0xff140000UL

#define KERNEL_DEFAULT 0x200000UL   // where Rockchip's loader puts uboot.img

// What we found and did, for the kernel to show. The order is the "index"
// in SMC_REPORT, so only add at the end (the kernel names them the same).
enum {
	R_STEPS,          // how many start-up steps finished
	R_X0, R_X1,       // what the loader passed us
	R_KERNEL,         // where we start the kernel
	R_FROM_LIST,      // 1 = that came from the loader's list
	R_TICKS_IN,       // counter when we started
	R_TICKS_OUT,      // counter when we left for the kernel
	R_CNTFRQ_WAS,     // counter frequency before we set it
	R_ECTLR_WAS,      // CPUECTLR before: bit 6 = in the coherency (SMP) club
	R_STIMER_WAS,     // secure timer 1 control before
	R_SGRF_WAS,       // peripheral security before (4 x 16 bits, 1 = secure)
	R_FIREWALL_WAS,   // which firewall regions were on before
	R_GIC_LINES,      // interrupt numbers the GIC has
	R_CORES,          // PMU power-down status at start: bits 0-3 = cores off
	R_CRASH_SLOT,     // our own crash: which vector (~0 = none)
	R_CRASH_ESR, R_CRASH_ELR, R_CRASH_FAR,
	R_SMCS,           // how many smc calls the kernel made
	R_LAST_SMC,       // the last function number
	R_COUNT
};
static uint64_t report[R_COUNT];

// SMC function numbers. Bits: 31 = "fast" call, 30 = 64-bit, 29-24 = owner
// (4 = standard, like PSCI; 2 = "silicon provider", the chip's own).
#define PSCI_VERSION      0x84000000
#define PSCI_SYSTEM_OFF   0x84000008
#define PSCI_SYSTEM_RESET 0x84000009
#define PSCI_FEATURES     0x8400000a
#define PSCI_CPU_OFF      0x84000002   // the calling core stops, until a CPU_ON
#define PSCI_CPU_ON       0xc4000003   // x1 = core (MPIDR), x2 = where, x3 = its x0
#define PSCI_ALREADY_ON   (~0UL - 3)   // -4
#define PSCI_INVALID      (~0UL - 1)   // -2
#define PSCI_FAILED       (~0UL - 5)   // -6
#define SMC_HELLO         0xc200ff00   // ours: who's there?
#define SMC_REPORT        0xc200ff01   // ours: x1 = index -> x0 = report[x1]
#define SMC_ROM_READ      0xc200ff02   // ours: x1 = offset -> x0 = boot ROM word, x1 = 1 if read
#define BOOT_ROM          0xffff0000UL
#define BOOT_ROM_SIZE     0x10000UL
#define HELLO_MAGIC       0x4d4f4e49544f52UL   // "MONITOR" in ASCII
#define NOT_SUPPORTED     (~0UL)       // -1, for PSCI and unknown calls alike

static void delay_us(uint32_t us)
{
	uint64_t t = SYSREG(cntpct_el0);
	while (SYSREG(cntpct_el0) - t < (uint64_t)us * 24)
		;
}

// 1. The CPU itself.
static void cpu_init(void)
{
	// CPUECTLR bit 6, SMPEN: take part in keeping the cores' caches
	// coherent. The Cortex-A35 manual says to set it before turning on
	// caches or the MMU, and only EL3 may. (ATF does it at reset.)
	uint64_t ectlr = SYSREG(s3_1_c15_c2_1);
	report[R_ECTLR_WAS] = ectlr;
	SET_SYSREG(s3_1_c15_c2_1, ectlr | 1 << 6);

	// The counter's frequency, as the kernel reads it: only EL3 can set it
	report[R_CNTFRQ_WAS] = SYSREG(cntfrq_el0);
	SET_SYSREG(cntfrq_el0, 24000000);
	__asm__ volatile("isb");
}

// 2. On Rockchip chips, the counter every core's timer reads (CNTPCT)
// comes from secure timer channel 1. The boot ROM already started it
// (that's how the kernel knows how long since power-on), so we leave it
// running if it is, and only start it if not. (ATF always restarts it.)
static void counter_init(void)
{
	uintptr_t ch1 = STIMER + 0x20;
	report[R_STIMER_WAS] = rd(ch1 + 0x10);
	if (rd(ch1 + 0x10) & 1)
		return;
	wr(ch1 + 0x10, 0);            // off while we set it up
	wr(ch1 + 0x00, 0xffffffff);   // count from the top, 64 bits
	wr(ch1 + 0x04, 0xffffffff);
	wr(ch1 + 0x10, 1);            // on, free-running
}

// 3. Out of reset, many peripherals only answer the secure world. The
// SGRF holds a bit per peripheral; the top 16 bits of each write say which
// of the bottom 16 to change (a "write mask", like the PMU switch).
static void peripherals_init(void)
{
	report[R_SGRF_WAS] = (uint64_t)(rd(SGRF + 0x10) & 0xffff) |
			     (uint64_t)(rd(SGRF + 0x14) & 0xffff) << 16 |
			     (uint64_t)(rd(SGRF + 0x18) & 0xffff) << 32 |
			     (uint64_t)(rd(SGRF + 0x1c) & 0xffff) << 48;
	for (uintptr_t con = 0x10; con <= 0x1c; con += 4)   // SOC_CON4-7
		wr(SGRF + con, 0xffff0000);   // every peripheral: non-secure
	wr(SGRF + 0x20, 0x00030000);      // SOC_CON8: the last two as well
	wr(SGRF + 0x0c, 0x000f0003);      // SOC_CON3: crypto engine non-secure

	// The DMA controller: its interrupts, channels and manager non-secure,
	// then reset it so it starts out that way
	wr(SGRF + 0x30, 0xffffffff);
	wr(SGRF + 0x34, 0xffffffff);
	wr(SGRF + 0x38, 0x000f000f);
	wr(SGRF + 0x3c, 0x00010001);
	wr(SGRF + 0x04, 1U << 28 | 1U << 12);   // SOC_CON1: DMA in reset...
	delay_us(5);
	wr(SGRF + 0x04, 1U << 28);              // ...and out again
}

// 4. The memory firewall has 8 regions, in megabytes. Region 0 covers our
// own first megabyte (we're at 0x10000): the kernel can't read or
// overwrite the monitor, even by accident.
static void firewall_init(void)
{
	report[R_FIREWALL_WAS] = rd(FIREWALL + 0x40);
	wr(FIREWALL + 0x00, (1 - 1) << 16 | 0);   // region 0: last MB 0, first MB 0
	wr(FIREWALL + 0x40, rd(FIREWALL + 0x40) | 1);
}

// 5. The GIC sorts interrupts into group 0 (secure, arrive as FIQs at EL3)
// and group 1 (non-secure). Out of reset they're all group 0, which the
// non-secure world can't even see. We have no interrupts of our own, so
// all go to group 1.
static void gic_init(void)
{
	uint32_t lines = ((rd(GICD + 0x004) & 0x1f) + 1) * 32;   // GICD_TYPER
	report[R_GIC_LINES] = lines;
	wr(GICD + 0x000, 0);   // distributor off while we change it
	for (uint32_t i = 0; i < lines / 32; i++)
		wr(GICD + 0x080 + i * 4, 0xffffffff);   // GICD_IGROUPR: group 1
	// The priority mask: the non-secure world can only lower it while it's
	// above 0x80 (the top half of priorities belongs to us). Out of reset
	// it's 0, which would block every interrupt, forever.
	wr(GICC + 0x004, 0xff);
	// Our side of this core's interface: no group 0 here, and no "bypass"
	// (the raw interrupt lines reaching the CPU while the GIC is off).
	wr(GICC + 0x000, 0xf << 5);
	// The kernel switches group 1 on itself (GICD_CTLR, GICC_CTLR).
}

// 6. The registers that set how the kernel starts at EL2. At reset most
// are "unknown", so we give each a clean value.
static void el2_init(void)
{
	// SCR_EL3, "secure configuration": NS (bit 0) = lower levels are non-
	// secure, bits 4-5 reserved ones, HCE (8) = `hvc` allowed, RW (10) =
	// EL2 runs 64-bit. IRQ, FIQ and SError stay with the kernel (bits 1-3
	// clear), and `smc` comes to us (SMD, bit 7, clear).
	SET_SYSREG(scr_el3, 1 | 3 << 4 | 1 << 8 | 1 << 10);
	SET_SYSREG(mdcr_el3, 0);              // no debug traps
	SET_SYSREG(sctlr_el2, 0x30c50830);    // EL2: MMU and caches off
	SET_SYSREG(hcr_el2, 1UL << 31);       // EL1 would be 64-bit too
	SET_SYSREG(cptr_el2, 0x33ff);         // EL2: no traps (the 1s are reserved)
	SET_SYSREG(cnthctl_el2, 3);           // EL1 may use the counter and timer
	SET_SYSREG(cntvoff_el2, 0);           // virtual counter = physical
	SET_SYSREG(vpidr_el2, SYSREG(midr_el1));
	SET_SYSREG(vmpidr_el2, SYSREG(mpidr_el1));
	SET_SYSREG(hstr_el2, 0);
	SET_SYSREG(vttbr_el2, 0);
	__asm__ volatile("isb");
}

// Rockchip's loader passes ATF's old "BL31 params" list in x0:
//   8-byte header (type 3, version 1), then 5 pointers; the 4th is the
//   kernel's "entry point info": 8-byte header, then the address to run.
// We check it looks right and fall back to where the loader always puts
// it if not.
static uint64_t kernel_address(uint64_t list)
{
	// In RAM, or (where this loader puts it) the on-chip SRAM at 0xff0e0000
	int in_sram = list >= 0xff0e0000 && list < 0xff0f0000;
	if (list == 0 || list & 7 || (list >= 0xf0000000 && !in_sram))
		return KERNEL_DEFAULT;
	const volatile uint8_t *h = (const volatile uint8_t *)list;
	if (h[0] != 3 || h[1] != 1)   // PARAM_BL31, version 1
		return KERNEL_DEFAULT;
	uint64_t ep = *(const volatile uint64_t *)(list + 8 + 3 * 8);
	if (ep == 0 || ep & 7 || (ep >= 0xf0000000 && !(ep >= 0xff0e0000 && ep < 0xff0f0000)))
		return KERNEL_DEFAULT;
	uint64_t pc = *(const volatile uint64_t *)(ep + 8);
	if (pc < 0x100000 || pc >= 0xf0000000 || pc & 3)
		return KERNEL_DEFAULT;
	report[R_FROM_LIST] = 1;
	return pc;
}

// ---- the other cores: PSCI CPU_ON --------------------------------------------
//
// Out of reset only core 0 runs our code; the boot ROM keeps cores 1-3
// waiting in a `wfe` loop of its own. To start one, as ATF does here:
//   - put a two-instruction jump to secondary_start (start.S) at the
//     start of the PMU's SRAM, and "remap" that SRAM to the address every
//     core starts from after reset (PMUSGRF_SOC_CON0 bit 13)
//   - switch the core's power off and on again (PMU_PWRDN_CON bit = core):
//     it starts from reset, at EL3, in our jump
//   - there it sets up its own copy of what's per core (secondary_main),
//     and drops to the kernel at EL2 where the kernel asked, with
//     interrupts masked and x0 = what the kernel passed

#define PMU_PWRDN_CON    (PMU + 0x18)
#define PMU_PWRDN_ST     (PMU + 0x20)
#define PMU_CPUAPM_CON(c) (PMU + 0xe0 + 4 * (c))
#define GRF_CPU_STATUS1  (GRF + 0x524)   // bits 0-3: core in wfe, 4-7: in wfi

uint64_t core_entry[4], core_arg[4];
volatile uint32_t core_started[4];
void secondary_start(void);   // start.S

// Power a core's domain to `off` (1) or on (0), and wait until it is
static int core_power(uint32_t core, uint32_t off)
{
	wr(PMU_PWRDN_CON, 1U << (core + 16) | off << core);
	__asm__ volatile("dsb sy");
	for (int i = 0; i < 1000; i++) {
		if ((rd(PMU_PWRDN_ST) >> core & 1) == off)
			return 0;
		delay_us(1);
	}
	return -1;
}

static uint64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t arg)
{
	uint32_t core = mpidr & 0xff;
	if ((mpidr & ~0xffUL & 0xffffff) || core < 1 || core > 3 || entry & 3)
		return PSCI_INVALID;
	// A core that went off wrote this with its cache off, straight to RAM:
	// drop any copy in ours first
	__asm__ volatile("dc civac, %0\ndsb sy" :: "r"(&core_started[core]) : "memory");
	if (core_started[core])
		return PSCI_ALREADY_ON;
	core_entry[core] = entry;
	core_arg[core] = arg;

	// ldr x0, 8 ; br x0 ; .quad secondary_start
	volatile uint32_t *sram = (volatile uint32_t *)PMUSRAM;
	sram[0] = 0x58000040;
	sram[1] = 0xd61f0000;
	sram[2] = (uint32_t)(uintptr_t)secondary_start;
	sram[3] = (uint32_t)((uintptr_t)secondary_start >> 32);
	wr(PMUSGRF + 0x00, 1U << (13 + 16) | 1U << 13);   // SOC_CON0: remap it
	__asm__ volatile("dsb sy");

	// Only while it's idle (in the ROM's wfe): never cut power mid-work
	for (int i = 0; i < 1000 && !(rd(GRF_CPU_STATUS1) & 0x11U << core); i++)
		delay_us(1);
	wr(PMU_CPUAPM_CON(core), 0xffff0000);   // no automatic power management
	if ((rd(PMU_PWRDN_ST) >> core & 1) == 0 && core_power(core, 1))
		return PSCI_FAILED;
	if (core_power(core, 0))
		return PSCI_FAILED;
	return 0;
}

// PSCI CPU_OFF, on the core itself: it sleeps here in the monitor, whose
// memory nobody overwrites, with interrupts masked. To the PMU that's an
// idle core, so a later CPU_ON can cut its power and start it afresh.
// (The kernel's own code may be replaced meanwhile: a new kernel.)
//
// First, its own level-1 data cache: still full of the kernel's data, some of
// it newer than RAM. Left there, the other cores would keep being handed
// these old copies (the caches answer each other), even after a new kernel
// has cleared that memory. So: stop caching, write every line back and drop
// it ("clean and invalidate", by set and way, the only way to say "all of
// it"), then leave the cores' coherency group (SMPEN off).
static void l1_flush(void)
{
	__asm__ volatile("msr csselr_el1, xzr\nisb");   // level 1, data
	uint64_t ccsidr = SYSREG(ccsidr_el1);
	uint32_t line = (ccsidr & 7) + 4;                 // log2 of the line size
	uint32_t ways = (ccsidr >> 3 & 0x3ff) + 1, sets = (ccsidr >> 13 & 0x7fff) + 1;
	uint32_t way_shift = __builtin_clz(ways - 1);     // ways at the top bits
	for (uint32_t w = 0; w < ways; w++)
		for (uint32_t s = 0; s < sets; s++)
			__asm__ volatile("dc cisw, %0" :: "r"((uint64_t)w << way_shift | (uint64_t)s << line));
	__asm__ volatile("dsb sy");
}

static void cpu_off(void)
{
	uint32_t core = SYSREG(mpidr_el1) & 0xff;
	__asm__ volatile("msr daifset, #0xf");
	SET_SYSREG(sctlr_el3, SYSREG(sctlr_el3) & ~(1UL << 2));   // C off
	__asm__ volatile("isb");
	l1_flush();
	SET_SYSREG(s3_1_c15_c2_1, SYSREG(s3_1_c15_c2_1) & ~(1UL << 6));   // SMPEN off
	__asm__ volatile("isb\ndsb sy");
	core_started[core] = 0;
	__asm__ volatile("dsb sy");
	for (;;)
		__asm__ volatile("wfi");
}

// A new core, at EL3 (start.S sent it here with its own stack). Returns
// where to go at EL2.
uint64_t secondary_main(uint32_t core)
{
	// Step 1, for this core: cache coherency and the counter's frequency
	SET_SYSREG(s3_1_c15_c2_1, SYSREG(s3_1_c15_c2_1) | 1 << 6);
	SET_SYSREG(cntfrq_el0, 24000000);
	// Step 5, for this core: its own 32 interrupts (the GIC keeps a copy of
	// these per core) to group 1, and its own interface as core 0's
	wr(GICD + 0x080, 0xffffffff);
	wr(GICC + 0x004, 0xff);
	wr(GICC + 0x000, 0xf << 5);
	el2_init();   // step 6
	core_started[core] = 1;
	__asm__ volatile("dsb sy");
	return core_entry[core];
}

uint64_t monitor_main(uint64_t x0, uint64_t x1, uint64_t ticks)
{
	report[R_CRASH_SLOT] = ~0UL;
	report[R_X0] = x0;
	report[R_X1] = x1;
	report[R_TICKS_IN] = ticks;
	report[R_CORES] = rd(PMU + 0x20);
	report[R_KERNEL] = KERNEL_DEFAULT;   // in case we crash before we know

	// The list lives in memory the loader owns: read it first
	report[R_KERNEL] = kernel_address(x0);

	cpu_init();          report[R_STEPS] = 1;
	counter_init();      report[R_STEPS] = 2;
	peripherals_init();  report[R_STEPS] = 3;
	firewall_init();     report[R_STEPS] = 4;
	gic_init();          report[R_STEPS] = 5;
	el2_init();          report[R_STEPS] = 6;

	report[R_TICKS_OUT] = SYSREG(cntpct_el0);
	return report[R_KERNEL] ? report[R_KERNEL] : KERNEL_DEFAULT;
}

// A crash during start-up (start.S, from any EL3 vector). Note it and try to
// start the kernel anyway: it shows the report, and the steps say how far
// we got. A second crash (in el2_init, say) and we stop.
uint64_t monitor_crashed(uint64_t slot)
{
	static int crashes;
	if (crashes++)
		for (;;)
			__asm__ volatile("wfe");
	report[R_CRASH_SLOT] = slot;
	report[R_CRASH_ESR] = SYSREG(esr_el3);
	report[R_CRASH_ELR] = SYSREG(elr_el3);
	report[R_CRASH_FAR] = SYSREG(far_el3);
	if (report[R_STEPS] < 6)
		el2_init();
	return report[R_KERNEL] ? report[R_KERNEL] : KERNEL_DEFAULT;
}

// PSCI SYSTEM_RESET, as ATF does it on this chip: every PLL to "slow mode"
// (straight from the 24 MHz crystal), then the CRU's global reset.
static void system_reset(void)
{
	wr(PMUCRU + 0x20, 3U << 16 | 0);   // GPLL: slow
	wr(CRU + 0xa0, 3U << 16 | 0);      // APLL (CPU)
	wr(CRU + 0xa0, 3U << 18 | 0);      // CPLL
	wr(CRU + 0xa0, 3U << 22 | 0);      // NPLL
	__asm__ volatile("dsb sy");
	wr(CRU + 0xb8, 0xfdb9);            // GLB_SRST_FST: reset everything
	__asm__ volatile("dsb sy");
	for (;;)
		__asm__ volatile("wfi");
}

// x[0..30]: the kernel's registers at the `smc`. Results go in x0-x3.
uint32_t probe_read(uint64_t address, uint32_t *ok);   // start.S

void smc_handler(uint64_t *x)
{
	uint64_t esr = SYSREG(esr_el3);
	if (esr >> 26 != 0x17) {   // not an smc (nothing else should reach us)
		x[0] = NOT_SUPPORTED;
		return;
	}
	uint32_t fn = (uint32_t)x[0];
	report[R_SMCS]++;
	report[R_LAST_SMC] = fn;
	switch (fn) {
	case PSCI_VERSION:
		x[0] = 1 << 16 | 0;   // 1.0
		break;
	case PSCI_FEATURES: {
		uint32_t asked = (uint32_t)x[1];
		x[0] = asked == PSCI_VERSION || asked == PSCI_FEATURES ||
		       asked == PSCI_SYSTEM_RESET || asked == PSCI_CPU_ON ||
		       asked == PSCI_CPU_OFF ? 0 : NOT_SUPPORTED;
		break;
	}
	case PSCI_SYSTEM_RESET:
		system_reset();
		break;
	case PSCI_CPU_ON:
		x[0] = cpu_on(x[1], x[2], x[3]);
		break;
	case PSCI_CPU_OFF:
		if ((SYSREG(mpidr_el1) & 0xff) == 0) {   // not core 0: someone must stay
			x[0] = PSCI_INVALID;
			break;
		}
		cpu_off();   // never returns
	case SMC_HELLO:
		x[0] = HELLO_MAGIC;
		x[1] = R_COUNT;
		break;
	case SMC_REPORT:
		x[0] = x[1] < R_COUNT ? report[x[1]] : NOT_SUPPORTED;
		break;
	case SMC_ROM_READ: {   // the boot ROM, read from the secure world
		uint32_t ok = 0;
		x[0] = x[1] < BOOT_ROM_SIZE ? probe_read(BOOT_ROM + (x[1] & ~3UL), &ok) : 0;
		x[1] = ok;
		break;
	}
	default:   // includes SYSTEM_OFF: that needs the power chip, over I2C
		x[0] = NOT_SUPPORTED;
		break;
	}
}
