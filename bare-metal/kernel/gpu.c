// The GPU: an Arm Mali-G31 ("Bifrost" generation), at 0xff400000.
//
// Arm doesn't publish its documentation; everything here follows Linux's
// open-source Panfrost driver (drivers/gpu/drm/panfrost) and Mesa's
// descriptions of the GPU's data structures (src/panfrost/genxml/v7.xml).
//
// How it's driven:
//   - The GPU is a separate processor that reads and writes RAM itself,
//     through its own MMU: page tables we build, in the same format as the
//     CPU's (a 4-level "LPAE" table), mapping our RAM 1:1.
//   - Work goes in as "jobs": 64-byte-aligned descriptors in RAM, each a
//     32-byte header (type, index, the address of the next job) and a
//     payload. We write the first job's address into a "job slot" register
//     and say START; the GPU walks the chain and raises a "done" bit.
//   - The GPU doesn't see what's in the CPU's caches (not "coherent"):
//     before a job we write our descriptors out to RAM ("clean"), and after
//     it we throw away our cached copy of what the GPU wrote ("invalidate").

#include <stdint.h>
#include "gpu.h"
#include "shaders.h"
#include "sysreg.h"

#define GPU 0xff400000UL
#define PMU 0xff000000UL

static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(GPU + off) = v; }
static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(GPU + off); }
static void pmu_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PMU + off) = v; }
static uint32_t pmu_rd(uint32_t off) { return *(volatile uint32_t *)(PMU + off); }
static uint64_t ticks(void) { return READ_SYSREG(cntpct_el0); }

// GPU registers (Panfrost's panfrost_regs.h)
#define GPU_ID            0x000
#define L2_FEATURES       0x004
#define MMU_FEATURES      0x014
#define AS_PRESENT        0x018
#define JS_PRESENT        0x01c
#define INT_RAWSTAT       0x020
#define INT_CLEAR         0x024
#define INT_MASK          0x028
#define GPU_CMD           0x030
#define FAULT_STATUS      0x03c
#define SHADER_PRESENT    0x100
#define TILER_PRESENT     0x110
#define L2_PRESENT        0x120
#define SHADER_READY      0x140
#define TILER_READY       0x150
#define L2_READY          0x160
#define SHADER_PWRON      0x180
#define TILER_PWRON       0x190
#define L2_PWRON          0x1a0
#define SHADER_CONFIG     0xf04
#define RESET_COMPLETED   (1U << 8)

#define JOB_INT_RAWSTAT   0x1000
#define JOB_INT_CLEAR     0x1004
#define JOB_INT_MASK      0x1008
#define JS(n, off)        (0x1800 + (n) * 0x80 + (off))
#define JS_STATUS         0x24
#define JS_HEAD_NEXT      0x40
#define JS_AFFINITY_NEXT  0x50
#define JS_CONFIG_NEXT    0x58
#define JS_COMMAND_NEXT   0x60

#define MMU_INT_RAWSTAT   0x2000
#define MMU_INT_CLEAR     0x2004
#define MMU_INT_MASK      0x2008
#define AS(n, off)        (0x2400 + (n) * 0x40 + (off))
#define AS_TRANSTAB       0x00
#define AS_MEMATTR        0x08
#define AS_COMMAND        0x18
#define AS_FAULTSTATUS    0x1c
#define AS_FAULTADDRESS   0x20
#define AS_STATUS         0x28

// Wait until (register & mask) == want, up to `us` microseconds
static int wait_for(uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t t0 = ticks();
	while ((rd(off) & mask) != want)
		if (ticks() - t0 > (uint64_t)us * 24)
			return -1;
	return 0;
}

// The GPU's memory: its page tables, jobs, and what it writes back. One
// 4 KB page per table; the jobs and results after them.
static struct {
	uint64_t level0[512], level1[512], level2[512];
	uint32_t job[3][32] __attribute__((aligned(64)));   // 128 bytes each
	uint64_t result[8] __attribute__((aligned(64)));
} mem __attribute__((aligned(4096)));

// Write our cached copy out to RAM, and drop it from the cache, so the
// next read comes from RAM (`dc civac`: clean and invalidate, by address)
static void flush(const void *p, uint64_t size)
{
	for (uint64_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + size; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int started;
static uint32_t shader_present;

int gpu_start(void (*say)(const char *fmt, ...))
{
	// 1. Its power domain (PMU_PWRDN_CON bit 15, a hiword-mask register),
	// then tell the bus it's no longer idle (PMU_BUS_IDLE_REQ bit 2), and
	// wait for the bus to agree (PMU_BUS_IDLE_ST: bit 2 idle, bit 18 ack)
	if (pmu_rd(0x20) & 1U << 15) {
		pmu_wr(0x18, 1U << (15 + 16));
		uint64_t t0 = ticks();
		while (pmu_rd(0x20) & 1U << 15)
			if (ticks() - t0 > 24000)
				return say("GPU power domain didn't come on\n"), -1;
	}
	pmu_wr(0x64, 1U << (2 + 16));
	uint64_t t0 = ticks();
	while (pmu_rd(0x6c) & (1U << 2 | 1U << 18))
		if (ticks() - t0 > 24000)
			return say("GPU bus stayed idle (PMU 0x6c = %08x)\n", pmu_rd(0x6c)), -1;

	uint32_t id = rd(GPU_ID);
	say("GPU_ID %08x: product %04x (Mali-G31 is 7093), revision r%up%u\n",
	    id, id >> 16, id >> 12 & 15, id >> 4 & 15);

	// 2. Soft reset: back to a known state, whatever ran before us
	wr(INT_MASK, 0);   // we poll; no interrupts
	wr(INT_CLEAR, RESET_COMPLETED);
	wr(GPU_CMD, 1);
	if (wait_for(INT_RAWSTAT, RESET_COMPLETED, RESET_COMPLETED, 10000))
		return say("reset didn't complete\n"), -1;
	wr(INT_CLEAR, 0xffffffff);
	wr(JOB_INT_MASK, 0);
	wr(MMU_INT_MASK, 0);
	// Panfrost's setting for this GPU (HW_FEATURE_TLS_HASHING)
	wr(SHADER_CONFIG, 1U << 17);

	// 3. Power up its parts, in order: L2 cache, shader core(s), tiler
	shader_present = rd(SHADER_PRESENT);
	uint32_t l2 = rd(L2_PRESENT), tiler = rd(TILER_PRESENT);
	wr(L2_PWRON, l2);
	if (wait_for(L2_READY, ~0U, l2, 20000))
		return say("L2 didn't power up\n"), -1;
	wr(SHADER_PWRON, shader_present);
	if (wait_for(SHADER_READY, ~0U, shader_present, 20000))
		return say("shader core didn't power up\n"), -1;
	wr(TILER_PWRON, tiler);
	if (wait_for(TILER_READY, ~0U, tiler, 20000))
		return say("tiler didn't power up\n"), -1;
	uint32_t mmu = rd(MMU_FEATURES);
	say("powered: L2 %x, shader cores %x, tiler %x; job slots %x, address spaces %x\n",
	    l2, shader_present, tiler, rd(JS_PRESENT), rd(AS_PRESENT));
	say("its MMU: %u-bit GPU addresses, %u-bit physical\n", mmu & 0xff, mmu >> 8 & 0xff);

	// 4. Its page tables: GPU address = physical address, for our 1 GB of
	// RAM, as 2 MB blocks. 4 levels (the GPU wants all 4): level 0 entry 0
	// -> level 1 entry 0 -> level 2's 512 blocks.
	//   bits 0-1  1 = block, 3 = next table
	//   bits 6-7  3 = GPU may read and write
	//   bits 2-4  0 = memory type 0 of MEMATTR below
	//   bits 8-9  2 = shared outside the GPU
	for (uint32_t i = 0; i < 512; i++) {
		mem.level0[i] = mem.level1[i] = 0;
		mem.level2[i] = (uint64_t)i << 21 | 3 << 6 | 2 << 8 | 1;
	}
	mem.level0[0] = (uintptr_t)mem.level1 | 3;
	mem.level1[0] = (uintptr_t)mem.level2 | 3;
	flush(&mem, sizeof mem);

	// Address space 0 uses them: table base | 3 (it's a table) | 4 (read
	// it through the GPU's cache); memory type 0 = 0x88, Panfrost's choice
	if (wait_for(AS(0, AS_STATUS), 1, 0, 100000))
		return say("GPU MMU busy\n"), -1;
	wr(AS(0, AS_TRANSTAB), (uint32_t)(uintptr_t)mem.level0 | 4 | 3);
	wr(AS(0, AS_TRANSTAB) + 4, 0);
	wr(AS(0, AS_MEMATTR), 0x88 | 0x8d << 8 | 0x88 << 16);
	wr(AS(0, AS_MEMATTR) + 4, 0);
	wr(AS(0, AS_COMMAND), 1);   // UPDATE: use them
	if (wait_for(AS(0, AS_STATUS), 1, 0, 100000))
		return say("GPU MMU update didn't finish\n"), -1;
	say("page tables at %lx: GPU addresses = ours, first 1 GB\n", (uintptr_t)mem.level0);
	started = 1;
	return 0;
}

// A "write value" job: no shader program, the job manager itself stores a
// value. Header word 4: bit 0 = 64-bit descriptors, bits 1-7 = type (2 =
// write value), bits 16-31 = its index in the chain. Payload at byte 32:
// the address (64 bits), what to write (word 2: 2 = the GPU's timestamp,
// 7 = a 64-bit number), and the number (words 4-5).
static void write_value_job(uint32_t *job, uint32_t index, uint64_t at,
			    uint32_t what, uint64_t value, const uint32_t *next)
{
	for (int i = 0; i < 32; i++)
		job[i] = 0;
	job[4] = 1 | 2 << 1 | index << 16;
	job[6] = (uint32_t)(uintptr_t)next;
	job[8] = (uint32_t)at, job[9] = (uint32_t)(at >> 32);
	job[10] = what;
	job[12] = (uint32_t)value, job[13] = (uint32_t)(value >> 32);
}

// Run a job chain on job slot 1 and wait for it (up to 100 ms). The
// settings: address space 0, priority 8, and clean + invalidate the GPU's
// caches at the start of the chain (so it sees what we wrote) and at the
// end (so what it wrote reaches RAM). Returns 0 if it finished.
static int run_chain(const void *first, uint64_t *took, void (*say)(const char *fmt, ...))
{
	wr(JOB_INT_CLEAR, 0xffffffff);
	wr(MMU_INT_CLEAR, 0xffffffff);
	uint64_t before = ticks();
	wr(JS(1, JS_HEAD_NEXT), (uint32_t)(uintptr_t)first);
	wr(JS(1, JS_HEAD_NEXT) + 4, 0);
	wr(JS(1, JS_AFFINITY_NEXT), shader_present);
	wr(JS(1, JS_AFFINITY_NEXT) + 4, 0);
	wr(JS(1, JS_CONFIG_NEXT), 0 | 8 << 16 | 3 << 8 | 3 << 12);
	wr(JS(1, JS_COMMAND_NEXT), 1);   // START
	// Done (bit 1) or failed (bit 17), for slot 1
	uint32_t raw;
	while (!((raw = rd(JOB_INT_RAWSTAT)) & (1U << 1 | 1U << 17)))
		if (ticks() - before > 24000 * 100)
			break;
	*took = ticks() - before;
	wr(JOB_INT_CLEAR, raw);
	if (raw & 1U << 1)
		return 0;
	say("job chain didn't finish: JOB_INT_RAWSTAT %08x, JS_STATUS %02x, GPU fault %08x,\n"
	    "  MMU int %08x, AS0 fault %08x at %08x\n", raw, rd(JS(1, JS_STATUS)),
	    rd(FAULT_STATUS), rd(MMU_INT_RAWSTAT), rd(AS(0, AS_FAULTSTATUS)),
	    rd(AS(0, AS_FAULTADDRESS)));
	return -1;
}

void gpu_job_test(void (*say)(const char *fmt, ...))
{
	if (!started && gpu_start(say))
		return;
	for (int i = 0; i < 8; i++)
		mem.result[i] = 0;
	// Job 1 writes a number; jobs 2 and 3, chained after it, write the
	// GPU's timestamp and its count of its own clock cycles. Those two only
	// run after GPU_CMD 5 (CYCLE_COUNT_START).
	wr(GPU_CMD, 5);
	write_value_job(mem.job[0], 1, (uintptr_t)&mem.result[0], 7, 0x00c0ffee12345678UL, mem.job[1]);
	write_value_job(mem.job[1], 2, (uintptr_t)&mem.result[1], 2, 0, mem.job[2]);
	write_value_job(mem.job[2], 3, (uintptr_t)&mem.result[2], 1, 0, 0);
	flush(&mem.job, sizeof mem.job + sizeof mem.result);

	uint64_t before = ticks(), took;
	int failed = run_chain(mem.job[0], &took, say);
	uint64_t after = before + took;
	flush(&mem.job, sizeof mem.job + sizeof mem.result);
	if (failed)
		return;
	say("job chain done in %lu us\n", took / 24);
	say("job 1 status %02x (1 = done): wrote %016lx  %s\n", mem.job[0][0],
	    mem.result[0], mem.result[0] == 0x00c0ffee12345678UL ? "(right)" : "(WRONG)");
	say("job 2 status %02x: the GPU's timestamp %lu; CPU's counter was %lu before, %lu after\n",
	    mem.job[1][0], mem.result[1], before, after);
	say("job 3 status %02x: GPU clock cycles counted %lu\n", mem.job[2][0], mem.result[2]);
}

// ---- compute jobs: running a shader program ---------------------------------
//
// A compute job (Mesa's v7.xml, "Compute Job"), 192 bytes:
//   0   the job header (type 4 = compute)
//   32  "invocation": the workgroup size and how many workgroups, packed
//       into one word with variable-width fields (pack_invocation)
//   40  parameters: "job task split", how the GPU divides the work
//   64  "draw": pointers to everything the shader may use. We fill in:
//         the renderer state (which shader, its settings)
//         push uniforms (the values the shader reads as u0, u1, ...)
//         thread storage (its scratch memory: we give it none)
//
// The renderer state descriptor (64 bytes) says where the program is, how
// many registers each thread needs (32 or 64: fewer registers means more
// threads at once), which registers the hardware fills in before the
// program starts (e.g. r60 = the thread's global ID), and how many 64-bit
// push uniforms to load.

static struct {
	uint32_t job[48] __attribute__((aligned(64)));
	uint32_t rsd[16] __attribute__((aligned(64)));
	uint32_t tls[8] __attribute__((aligned(64)));
	uint64_t push[8] __attribute__((aligned(64)));
	uint32_t out[1024] __attribute__((aligned(64)));
} cmem;

static uint32_t log2_ceil(uint32_t v)
{
	uint32_t r = 0;
	while ((1U << r) < v)
		r++;
	return r;
}

// Workgroup size x,y,z and number of workgroups x,y,z, each stored as
// (value - 1) in just enough bits, one after another (Mesa's
// pan_pack_work_groups_compute)
static void pack_invocation(uint32_t *w, const uint32_t v[6])
{
	uint32_t shift[7] = {0}, packed = 0;
	for (int i = 0; i < 6; i++) {
		packed |= (v[i] - 1) << shift[i];
		shift[i + 1] = shift[i] + log2_ceil(v[i]);
	}
	w[0] = packed;
	// size y, size z, groups x, y, z shifts; then "thread group split",
	// which for compute must equal the groups-x shift
	w[1] = shift[1] | shift[2] << 5 | shift[3] << 10 | shift[4] << 16 |
	       shift[5] << 22 | shift[3] << 28;
}

// Run `code` (Mali machine code) as a compute job: `groups` workgroups of
// `size` threads, with push uniforms push[0..npush-1]
static int run_compute(const uint8_t *code, uint32_t code_size, uint32_t regs, uint64_t preload,
		       uint32_t fau, uint32_t size, uint32_t groups, uint64_t *took,
		       void (*say)(const char *fmt, ...))
{
	uint32_t *j = cmem.job, *r = cmem.rsd;
	for (int i = 0; i < 48; i++)
		j[i] = 0;
	for (int i = 0; i < 16; i++)
		r[i] = 0;
	for (int i = 0; i < 8; i++)
		cmem.tls[i] = 0;

	// Renderer state
	r[0] = (uint32_t)(uintptr_t)code;
	r[4] = (regs <= 32 ? 2U : 0U) << 12;   // registers per thread: 2 = 32, 0 = 64
	// Preload: which of r55-r62 the hardware fills in (bit 7 + n - 55), and
	// how many 64-bit push uniforms
	uint32_t pre = 0;
	for (int reg = 55; reg <= 62; reg++)
		if (preload >> reg & 1)
			pre |= 1U << (7 + reg - 55);
	r[12] = pre | ((fau + 1) / 2) << 15;

	// Thread storage: no scratch memory, no workgroup-shared memory
	// ("WLS instances" = log2 of 0x80000000, meaning none)
	cmem.tls[1] = 31;

	// The job
	j[4] = 1 | 4 << 1 | 1 << 16;   // 64-bit, compute, index 1
	uint32_t v[6] = {size, 1, 1, groups, 1, 1};
	pack_invocation(&j[8], v);
	j[10] = (log2_ceil(size + 1) + 1 + 1) << 26;   // job task split
	uint32_t *d = &j[16];   // draw
	d[0] = 3;   // the defaults: "four components per vertex", 64-bit
	d[12] = (uint32_t)(uintptr_t)cmem.push;
	d[14] = (uint32_t)(uintptr_t)cmem.rsd;
	d[28] = (uint32_t)(uintptr_t)cmem.tls;

	flush(code, code_size);
	flush(&cmem, sizeof cmem);
	int failed = run_chain(cmem.job, took, say);
	flush(&cmem, sizeof cmem);
	if (!failed && (cmem.job[0] & 0xff) != 1) {
		say("job status %02x (not 1, done)\n", cmem.job[0]);
		failed = -1;
	}
	return failed;
}

void gpu_compute_test(void (*say)(const char *fmt, ...))
{
	if (!started && gpu_start(say))
		return;
	const uint32_t size = 16, groups = 4, n = size * groups;
	for (uint32_t i = 0; i < n + 4; i++)
		cmem.out[i] = 0xdeadbeef;
	cmem.push[0] = (uintptr_t)cmem.out;   // the shader's u0: where to write
	uint64_t took;
	if (run_compute(shader_ids, sizeof shader_ids, SHADER_IDS_REGS, SHADER_IDS_PRELOAD,
			SHADER_IDS_FAU, size, groups, &took, say))
		return;
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < n; i++)
		if (cmem.out[i] != i * i + 7)
			wrong++;
	say("compute job done in %lu us: %u threads (%u workgroups of %u), %u wrong\n",
	    took / 24, n, groups, size, wrong);
	say("out[0..7]:");
	for (uint32_t i = 0; i < 8; i++)
		say(" %u", cmem.out[i]);
	say("\nout[%u..%u]: %u %u, then (untouched) %08x\n", n - 2, n - 1,
	    cmem.out[n - 2], cmem.out[n - 1], cmem.out[n]);
}
