// What happens when the CPU stops normal execution: vectors.S saves the
// registers and calls exception(). Interrupts and system calls are handled
// and return; anything else is a crash, and we show everything we know.

#include "buttons.h"
#include "console.h"
#include "exception.h"
#include "gic.h"
#include "sysreg.h"

#define RED    0x801818
#define WHITE  0xf0f0f0
#define PINK   0xf0b0b0
#define YELLOW 0xf0d020

volatile uint32_t syscall_count, last_syscall;

// The exception registers exist once per level; read the ones for ours.
#define EL_REG(name) (current_el() == 2 ? READ_SYSREG(name##_el2) : READ_SYSREG(name##_el1))

// ESR bits 31-26, the "exception class": the CPU's own summary of what happened
static const char *class_name(uint32_t ec)
{
	switch (ec) {
	case 0x00: return "Undefined instruction";
	case 0x01: return "WFI/WFE trapped";
	case 0x07: return "FP/SIMD used, but off";
	case 0x0e: return "Illegal execution state";
	case 0x15: return "System call (svc)";
	case 0x16: return "Hypervisor call (hvc)";
	case 0x17: return "Secure monitor call (smc)";
	case 0x18: return "System register trapped";
	case 0x20: return "Instruction fetch failed (lower)";
	case 0x21: return "Instruction fetch failed";
	case 0x22: return "Misaligned PC";
	case 0x24: return "Data access failed (lower)";
	case 0x25: return "Data access failed";
	case 0x26: return "Misaligned stack pointer";
	case 0x2f: return "SError (async hardware error)";
	case 0x3c: return "Breakpoint instruction (brk)";
	default:   return "Unknown exception class";
	}
}

// ESR bits 5-0 for aborts, the "fault status code": why the access failed
static const char *fault_name(uint32_t fsc)
{
	if (fsc <= 0x03) return "address too big";
	if (fsc <= 0x07) return "no page table entry";
	if (fsc >= 0x09 && fsc <= 0x0b) return "access flag fault";
	if (fsc >= 0x0d && fsc <= 0x0f) return "permission fault";
	if (fsc == 0x10) return "bus error, nothing there?";
	if (fsc == 0x21) return "misaligned access";
	return "other fault";
}

static void restart(void)
{
	// Ask ARM Trusted Firmware (one level up, EL3) to reset the board:
	// PSCI SYSTEM_RESET, the same call Linux makes for `reboot`.
	register uint64_t x0 __asm__("x0") = 0x84000009;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	for (;;)
		__asm__ volatile("wfe");
}

static void crash_screen(uint64_t slot, struct regs *r)
{
	static const char *const from[4] = {
		"same level, SP0", "same level, SPx", "lower level, 64", "lower level, 32" };
	static const char *const kind[4] = { "sync", "IRQ", "FIQ", "SError" };
	static int crashing;

	if (crashing)   // crashed while drawing the crash screen: give up
		for (;;)
			__asm__ volatile("wfe");
	crashing = 1;

	uint64_t esr = EL_REG(esr), elr = EL_REG(elr), far = EL_REG(far);
	uint64_t spsr = EL_REG(spsr);
	uint32_t ec = esr >> 26 & 0x3f;
	int abort = ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25;

	console_init(WHITE, RED);
	console_color(YELLOW);
	kputs("The kernel crashed\n");
	console_color(WHITE);
	kprintf("%s\n", slot % 4 ? kind[slot % 4] : class_name(ec));
	console_color(PINK);
	if (abort)
		kprintf("%s, %s\n", fault_name(esr & 0x3f),
			ec >= 0x24 ? (esr >> 6 & 1 ? "write" : "read") : "fetch");
	else
		kprintf("%s, %s\n", from[slot / 4], kind[slot % 4]);

	console_color(PINK);  kputs("Where ");
	console_color(WHITE); kprintf("%016lx\n", elr);
	console_color(PINK);  kputs("Addr  ");
	console_color(WHITE);
	if (abort || ec == 0x22)
		kprintf("%016lx\n", far);
	else
		kputs("-\n");
	console_color(PINK);  kputs("ESR ");
	console_color(WHITE); kprintf("%08lx ", esr);
	console_color(PINK);  kputs("SPSR ");
	console_color(WHITE); kprintf("%08lx\n", spsr);

	// x0-x30 and sp, two to a line: " 0 0000000002000000  1 ..."
	for (uint32_t i = 0; i < 32; i++) {
		console_color(PINK);
		if (i == 31)
			kputs("sp");
		else
			kprintf("%2u", i);
		console_color(WHITE);
		kprintf(" %016lx%s", i == 31 ? r->sp : r->x[i], i % 2 ? "\n" : " ");
	}

	console_color(YELLOW);
	kputs("Press any button to restart");

	while (buttons_pressed())   // let go of whatever caused it first
		;
	while (!buttons_pressed())
		;
	restart();
}

// Who handles which interrupt. The RK3326 uses numbers up to about 160.
#define MAX_IRQ 192
static void (*handlers[MAX_IRQ])(uint32_t id);

void irq_register(uint32_t id, void (*handler)(uint32_t id), uint8_t priority)
{
	handlers[id] = handler;
	gic_enable(id, priority);
}

// An interrupt: ask the GIC which one, handle it, tell the GIC we're done.
static void irq(void)
{
	uint32_t id = gic_ack();
	if (id == GIC_SPURIOUS)   // it went away before we asked
		return;
	if (id < MAX_IRQ && handlers[id])
		handlers[id](id);
	gic_done(id);
}

void exception(uint64_t slot, struct regs *r)
{
	if (slot == 5) {   // IRQ, from our own level (offset 0x280)
		irq();
		return;
	}
	uint64_t esr = EL_REG(esr);
	// A system call from our own level, on a sync exception: count it and go
	// back. ELR already points at the instruction after the `svc`.
	if (slot % 4 == 0 && slot < 8 && esr >> 26 == 0x15) {
		last_syscall = esr & 0xffff;   // the #n in `svc #n`
		syscall_count++;
		return;
	}
	crash_screen(slot, r);
}
