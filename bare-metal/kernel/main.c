// Milestone 10: a report on the display, and who's on the I2C buses.
//
// First the kernel describes the machine it woke up on: which CPU, which
// privilege level, the timer, the screen U-Boot handed us, and the device
// tree, with a live clock. Press any button and it switches to a button test
// that lights up each button as you press it. Hold FN and press A, B, X, Y or
// START to cause an exception on purpose: four crash, the system call returns.
//
// Before all that, it times three small jobs with the caches off (as U-Boot
// left them), turns on the MMU and caches, and times them again. Then it
// switches on interrupts: the timer interrupts 60 times a second, and in
// between, the CPU sleeps. On the button test the timer slows to once a
// second, and the buttons themselves interrupt (and wake) the CPU.
//
// On the info page, VOL+ and VOL- set the backlight, and it switches off by
// itself after 30 seconds without a button press. B shows the power page:
// the battery and the power chip's outputs, read over I2C. X shows how
// U-Boot set up the display, and Y scans the I2C buses for chips.

#include <stdint.h>
#include "backlight.h"
#include "board.h"
#include "buttons.h"
#include "console.h"
#include "display.h"
#include "dump.h"
#include "exception.h"
#include "gic.h"
#include "gpio.h"
#include "mmu.h"
#include "monitor.h"
#include "ram.h"
#include "rom.h"
#include "usb.h"
#include "net.h"
#include "log.h"
#include "loader.h"
#include "rk817.h"
#include "scan.h"
#include "screen.h"
#include "speed.h"
#include "sysreg.h"
#include "timer.h"

#define KEY    0x304050   // a button that isn't pressed

extern char _start[], _end[];

static void label(const char *name)
{
	console_color(GREY);
	uint32_t n = 0;
	for (; name[n]; n++)
		kputc(name[n]);
	for (; n < 8; n++)   // pad to 8 columns so the values line up
		kputc(' ');
	console_color(WHITE);
}

// ---- the device tree --------------------------------------------------------
// U-Boot passed us a "flattened device tree": a description of all the
// hardware, in a simple binary format, with every number stored big-endian.
// We walk it to find the board's name, the "model" property of the root node.

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static const char *dt_model(const uint8_t *dt)
{
	const uint8_t *p = dt + be32(dt + 8);                 // the structure block
	const char *strings = (const char *)dt + be32(dt + 12); // property names
	int depth = 0;
	for (;;) {
		uint32_t token = be32(p);
		p += 4;
		if (token == 1) {            // begin node: name follows, padded to 4 bytes
			depth++;
			while (*p++)
				;
			p = (const uint8_t *)(((uintptr_t)p + 3) & ~3UL);
		} else if (token == 2) {     // end node
			depth--;
		} else if (token == 3) {     // property: length, name offset, value
			uint32_t len = be32(p), name = be32(p + 4);
			if (depth == 1 && same(strings + name, "model"))
				return (const char *)p + 8;
			p += 8 + ((len + 3) & ~3U);
		} else if (token != 4) {     // 4 = nothing; 9 = end; anything else = broken
			return 0;
		}
	}
}

// ---- the CPU ----------------------------------------------------------------

static const char *cpu_name(uint64_t midr)
{
	if ((midr >> 24) != 0x41)
		return "not ARM-designed";
	switch ((midr >> 4) & 0xfff) {
	case 0xd03: return "Cortex-A53";
	case 0xd04: return "Cortex-A35";
	case 0xd05: return "Cortex-A55";
	}
	return "ARM core";
}

static uint64_t ticks_per_second;
static struct speed before, after;
static int irqs_work;   // did the timer interrupt actually arrive?

// Sleep until an interrupt (Wait For Interrupt), if they're working.
static void wait(void)
{
	if (irqs_work)
		__asm__ volatile("wfi");
}

static void interrupts_on(void)
{
	if (current_el() == 2) {
		// HCR_EL2.IMO: IRQs are for EL2 (us). Without it they'd be meant for
		// EL1, and the CPU never interrupts a higher level for a lower one.
		uint64_t hcr = READ_SYSREG(hcr_el2);
		__asm__ volatile("msr hcr_el2, %0\nisb" : : "r"(hcr | 1UL << 4));
	}
	gic_init();
	timer_start(60);   // registers its handler, and unmasks IRQs: I = 0
}

static uint64_t now(void)
{
	return READ_SYSREG(cntpct_el0);
}

static void show_light(uint32_t row, uint32_t percent, int dark)
{
	console_goto(8, row);
	console_color(WHITE);
	if (dark)
		kprintf("off (press to wake)       ");
	else
		kprintf("%u%%, duty %u of %u   ", percent, backlight_period() * percent / 100,
			backlight_period());
}

static void speed_line(const char *name, uint64_t off, uint64_t on)
{
	uint64_t per_us = ticks_per_second / 1000000;
	label(name);
	if (!off) {   // not measured (see kmain)
		kprintf("      -%7luus\n", on / per_us);
		return;
	}
	kprintf("%7luus%7luus", off / per_us, on / per_us);
	console_color(GREEN);
	uint64_t x10 = on ? off * 10 / on : 0;   // how many times faster, 1 decimal
	kprintf("%5lu.%lux\n", x10 / 10, x10 % 10);
}

static void show_info(uintptr_t device_tree, uint64_t start_ticks)
{
	uint64_t midr = READ_SYSREG(midr_el1);

	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("Hello from our own kernel!\n");

	label("CPU");
	kprintf("%s r%dp%d\n", cpu_name(midr), (int)(midr >> 20) & 0xf, (int)midr & 0xf);
	label("Level");
	kprintf("EL%u\n", current_el());
	// DAIF: 1 = masked (blocked). D debug, A SError, I IRQ, F FIQ
	uint64_t daif = READ_SYSREG(daif);
	label("Masks");
	kprintf("D%lu A%lu I%lu F%lu (1 = blocked)\n", daif >> 9 & 1, daif >> 8 & 1, daif >> 7 & 1, daif >> 6 & 1);
	label("Vectors");
	kprintf("%p\n", current_el() == 2 ? READ_SYSREG(vbar_el2) : READ_SYSREG(vbar_el1));
	label("Timer");
	kprintf("%lu ticks a second\n", ticks_per_second);
	label("Booted");
	kprintf("%lu ms after power-on\n", start_ticks * 1000 / ticks_per_second);
	label("Kernel");
	kprintf("%p, %lu bytes\n", _start, (uint64_t)(_end - _start));
	label("Screen");
	kprintf("%ux%u %s\n", screen.width, screen.height, format_name(screen.format));
	label("");
	kprintf("at %p, pitch %u\n", screen.fb, screen.pitch);

	const uint8_t *dt = (const uint8_t *)device_tree;
	label("Tree");
	if (dt && be32(dt) == 0xd00dfeed) {
		kprintf("%p, %u bytes\n", dt, be32(dt + 4));
		const char *model = dt_model(dt);
		console_color(GREY);
		kputs("Model ");   // 2 short of the usual 8 columns, so the name fits
		console_color(WHITE);
		kputs(model ? model : "(none)");
		kputc('\n');
	} else if (!dt) {
		kputs("none: U-Boot didn't start us\n");
		console_color(GREY);
		kputs("Loader ");
		console_color(GREEN);
		kputs(spl_report() ? "our SPL, our EL3, then us\n"
		      : monitor_ours() ? "Rockchip's, our EL3, then us\n"
				       : "Rockchip's, ARM TF, then us\n");
	} else {
		kprintf("no device tree at %p\n", dt);
	}

	uint64_t sctlr = current_el() == 2 ? READ_SYSREG(sctlr_el2) : READ_SYSREG(sctlr_el1);
	label("Memory");
	kprintf("MMU %s, caches %s\n", sctlr & 1 ? "on" : "off", sctlr & 4 ? "on" : "off");
	label("Speed");
	console_color(GREY);
	kputs("      off       on  faster\n");
	speed_line("Code", before.code, after.code);
	speed_line("RAM", before.ram, after.ram);
	speed_line("Screen", before.screen, after.screen);
}

// ---- the button test --------------------------------------------------------

static uint32_t length(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void draw_button(const struct button *b, int down)
{
	uint32_t fill = down ? YELLOW : KEY;
	uint32_t ink = down ? BG : WHITE;
	rect(b->x, b->y, b->w, b->h, fill);
	text_at(b->x + (b->w - length(b->name) * CELL_W) / 2, b->y + (b->h - 16) / 2,
		b->name, ink, fill);
}

static uint32_t bit_of(const char *name)
{
	for (uint32_t i = 0; i < button_count; i++)
		if (same(buttons[i].name, name))
			return 1U << i;
	return 0;
}

// A 4-byte read from an odd address in device memory (GPIO1's registers).
// Normal memory allows that now that the MMU is on; device memory never does:
// an alignment fault.
static void misaligned_read(void)
{
	uint32_t value;
	uintptr_t odd = 0xff250000UL + 0x50 + 1;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(odd));
}

// 0x40000000 is past the end of RAM, and our page tables say nothing is
// there: a translation fault.
static void unmapped_read(void)
{
	uint32_t value;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(0x40000000UL));
}

// ---- the power page ---------------------------------------------------------

static void padded(const char *s, uint32_t width)
{
	uint32_t n = 0;
	for (; s[n]; n++)
		kputc(s[n]);
	for (; n < width; n++)
		kputc(' ');
}

static void millivolts(uint32_t mv)
{
	kprintf("%u.%03u V", mv / 1000, mv % 1000);
}

static void draw_power(void)
{
	console_goto(0, 2);
	uint32_t id;
	label("Chip");
	if (rk817_id(&id)) {
		console_color(YELLOW);
		kputs("no answer on I2C0 at 0x20\n");
		return;
	}
	kprintf("RK%x, revision %u (I2C0, 0x%x)\n", id >> 4, id & 0xf, RK817_ADDR);
	kputc('\n');

	struct battery b;
	if (rk817_battery(&b) == 0) {
		label("Battery");
		millivolts(b.mv);
		console_clear_eol();
		kputc('\n');
		label("Current");
		kprintf("%d mA %s", b.ma, b.ma < 0 ? "out" : "in");
		console_clear_eol();
		kputc('\n');
		label("Charger");
		if (b.plugged_in) {
			if (b.charger_mv) {
				millivolts(b.charger_mv);
				kputs(", ");
			}
			kputs(rk817_charge_status(b.status));
		} else {
			kputs("not plugged in");
		}
		console_clear_eol();
		kputc('\n');
	}
	kputc('\n');

	struct rail rails[RK817_RAILS];
	if (rk817_rails(rails) == 0)
		for (uint32_t i = 0; i < RK817_RAILS; i++) {
			console_color(rails[i].on ? WHITE : GREY);
			padded(rails[i].output, 6);
			padded(rails[i].name, 14);
			if (rails[i].mv)
				millivolts(rails[i].mv);
			else
				kputs("       ");
			kputs(rails[i].on ? "  on \n" : "  off\n");
		}
}

// The battery and the power chip's outputs, read over I2C once a second
static void power_page(void)
{
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("The power chip\n");
	console_goto(0, 22);
	kputs("A: button test   VOL+/-: light");
	timer_start(1);
	uint32_t light = backlight_get(), held = buttons_pressed();
	for (;;) {
		draw_power();
		uint64_t ticks = timer_ticks;
		while (timer_ticks == ticks) {   // sleep until the next second
			wait();
			uint32_t pressed = buttons_pressed(), down = pressed & ~held;
			held = pressed;
			if (down & bit_of("A")) {
				while (buttons_pressed())
					wait();
				return;
			}
			// change the brightness, and watch the current follow
			if (down & bit_of("VOL+"))
				backlight_set(light = light >= 90 ? 100 : light + 10);
			if (down & bit_of("VOL-"))
				backlight_set(light = light <= 20 ? 10 : light - 10);
		}
	}
}

// ---- the display report and the I2C scan -------------------------------------

// Wait for a button to go down, and return which
static uint32_t next_press(void)
{
	uint32_t held = buttons_pressed();
	for (;;) {
		wait();
		uint32_t pressed = buttons_pressed(), down = pressed & ~held;
		held = pressed;
		if (down)
			return down;
	}
}

static void let_go(void)
{
	while (buttons_pressed())
		wait();
}

// The display report. START takes the display over: our own code resets
// and rebuilds the whole pipeline, and then the report shows what our code
// set up, with a log page first. If the screen stays dark after that,
// START again restarts the console.
static void display_report(void)
{
	timer_start(1);
	int ours = display_done();   // without U-Boot, it already ran
	uint32_t pages = ours ? DUMP_PAGES + 1 : DUMP_PAGES;
	uint32_t page = ours ? DUMP_PAGES : 0;   // our log first
	for (;;) {
		if (page == DUMP_PAGES)
			display_takeover_log();
		else
			dump_page(page);
		console_goto(0, 22);
		console_color(YELLOW);
		kputs(ours ? "L/R page  A buttons  START restart" : "L/R page  A buttons  START take over");
		uint32_t down = next_press();
		if (down & bit_of("A"))
			break;
		if (down & bit_of("START")) {
			if (ours)
				restart();
			let_go();
			display_start(1);
			ours = 1;
			pages = DUMP_PAGES + 1;
			page = DUMP_PAGES;   // the log first
			continue;
		}
		if (down & bit_of("R"))
			page = (page + 1) % pages;
		if (down & bit_of("L"))
			page = (page + pages - 1) % pages;
	}
	let_go();
}

static void scan_page(void)
{
	timer_start(1);
	i2c_scan_page();
	while (!(next_press() & bit_of("A")))
		;
	let_go();
}

// What Rockchip's RAM setup did; Y saves its registers to the card
// A page with something to save to the card (Y): the RAM registers, the ROM
static void save_screen(void (*page)(void), const char *(*save)(void), const char *saved)
{
	page();
	for (;;) {
		uint32_t down = next_press();
		if (down & bit_of("A"))
			break;
		if (down & bit_of("Y")) {
			console_goto(0, 21);
			console_color(YELLOW);
			kputs("Saving...                     ");
			const char *err = save();
			console_goto(0, 21);
			console_color(err ? ALERT : GREEN);
			kprintf("%s                ", err ? err : saved);
			if (err) {
				console_goto(0, 19);
				console_color(WHITE);
				ram_card_info();
			}
		}
	}
	let_go();
}

// USB: start the controller once, then keep answering the Mac. The driver
// polls, so this loop never sleeps while the page is open.
static void usb_screen(void)
{
	console_init(WHITE, BG);
	uint64_t shown = 0;
	while (buttons_pressed())
		usb_poll();
	for (;;) {
		usb_poll();
		if (now() - shown > ticks_per_second / 10) {
			shown = now();
			usb_page();
			net_lines(15);
			log_lines(20);
			loader_lines(21);
			console_goto(0, 22);
			console_color(YELLOW);
			kputs("B: back  (ping 192.168.36.1)");
		}
		if (buttons_pressed() & bit_of("B"))
			break;
	}
	while (buttons_pressed())
		usb_poll();
}

// What's at EL3 (Y tries to read the monitor's memory); R: our SPL
static void el3_page(void)
{
	timer_start(1);
	monitor_page();
	int spl = 0;
	for (;;) {
		uint32_t down = next_press();
		if (down & bit_of("A"))
			break;
		if (down & bit_of("Y") && monitor_ours() && !spl)
			monitor_peek(18);
		if (down & (bit_of("L") | bit_of("R"))) {
			spl = !spl;
			if (spl)
				spl_page();
			else
				monitor_page();
		}
	}
	let_go();
}

static void button_test(void)
{
	console_init(WHITE, BG);
	console_color(YELLOW);
	kputs("Press the buttons\n");
	console_color(GREY);
	kputs("Hold FN + A B X Y START: tests");
	for (uint32_t i = 0; i < button_count; i++)
		draw_button(&buttons[i], 0);

	// Only a heartbeat from the timer now: the buttons wake us themselves.
	timer_start(1);

	uint32_t shown = 0;
	uint64_t by_button = 0, by_timer = 0, presses = 0;
	uint64_t irqs_seen = button_irqs, ticks_seen = timer_ticks;
	for (;;) {
		wait();
		// what woke us?
		if (button_irqs != irqs_seen)
			by_button++;
		else if (timer_ticks != ticks_seen)
			by_timer++;
		irqs_seen = button_irqs;
		ticks_seen = timer_ticks;

		console_color(WHITE);
		console_goto(0, 19);
		kprintf("Woke by buttons %lu, timer %lu   ", by_button, by_timer);

		uint32_t pressed = buttons_pressed();
		for (uint32_t down = pressed & ~shown; down; down &= down - 1)
			presses++;
		// A clean press and release is 2 changes. More means the switch
		// "bounced": its contacts rattled, and each rattle is a change.
		console_goto(0, 22);
		console_color(GREY);
		kprintf("IRQs %lu changes %lu presses %lu   ", irqs_seen, button_changes, presses);
		if (pressed == shown)
			continue;
		for (uint32_t i = 0; i < button_count; i++) {
			uint32_t bit = 1U << i;
			if ((pressed ^ shown) & bit)
				draw_button(&buttons[i], pressed & bit);
			if (pressed & bit & ~shown) {   // just went down: say what it is
				const struct button *b = &buttons[i];
				console_goto(0, 20);
				console_color(GREY);
				kprintf("%s = GPIO%u pin %u            ", b->dt_label, b->bank, b->pin);
			}
		}
		// the raw input registers: watch a bit drop to 0 as you press
		console_goto(0, 21);
		console_color(GREY);
		kprintf("1:%08x 2:%08x 3:%08x", gpio_read(1), gpio_read(2), gpio_read(3));
		uint32_t fn = bit_of("FN");
		if (pressed & fn) {
			uint32_t now_down = pressed & ~shown;
			if (now_down & bit_of("A"))
				__asm__ volatile("brk #1");          // breakpoint
			if (now_down & bit_of("B"))
				__asm__ volatile("udf #0");          // permanently undefined
			if (now_down & bit_of("X"))
				misaligned_read();
			if (now_down & bit_of("START"))
				unmapped_read();
			if (now_down & bit_of("Y")) {
				__asm__ volatile("svc #42" ::: "memory");   // comes back
				console_goto(0, 20);
				console_color(GREEN);
				kprintf("svc #%u went and came back (%u)    ", last_syscall, syscall_count);
			}
		}
		shown = pressed;
	}
}

// Without U-Boot, nothing has set up the screen: we make our own. First
// just where it lives, so the MMU can map it...
// (Before the picture is there, the backlight is our only signal: steady
// full light = crashed; with make FLASHES=1, also one flash per step.)
static void own_screen(void)
{
	screen.fb = (volatile uint8_t *)0x3de00000UL;   // where U-Boot put it
	screen.width = 640;
	screen.height = 480;
	screen.pitch = 640 * 3;
	screen.format = FMT_RGB888;
	screen.vop = 0xff460000UL;
	screen.window = 1;
}

// ...then, with the MMU on (much faster screen writes), switch it all on
static void own_screen_start(void)
{
	if (board_init() != 0)
		return;   // no power chip: no backlight, no panel. Stay dark.
#ifdef FLASHES
	backlight_set(100);   // flash 1: the power chip answered
	uint64_t t = now();
	while (now() - t < 24000000 / 1000 * 150)
		;
	backlight_set(0);
	while (now() - t < 24000000 / 1000 * 600)
		;
#endif
	display_start(0);     // (flashes 2-8, one per step,) then the picture
}

// uboot = 1 if U-Boot loaded us, 0 if Rockchip's loader did (start.S)
uint64_t boot_kernel, boot_menu;   // boot timeline: we started, menu ready
extern uint64_t boot_picture;     // display.c

void kmain(uintptr_t device_tree, uint64_t uboot)
{
	boot_kernel = now();
	vectors_install();   // from here on, crashes come to us, not U-Boot

	if (!uboot)
		own_screen();
	else if (!screen_init())
		return;  // no screen to draw on; start.S parks the CPU

	uint64_t start_ticks = now();
	ticks_per_second = READ_SYSREG(cntfrq_el0);
	if (!ticks_per_second)
		ticks_per_second = 24000000;   // what the RK3326 uses

	// The same work, before and after turning on the MMU and caches. The
	// "before" run costs a third of a second, so only with make SPEEDTEST=1.
#ifdef SPEEDTEST
	speed_test(&before, BG);
#endif
	// Our page tables are for EL2. ARM TF should start us there without
	// U-Boot too, but if it doesn't, run without the MMU rather than crash.
	if (current_el() == 2)
		mmu_init((uintptr_t)screen.fb, screen.pitch * screen.height);
	speed_test(&after, BG);   // this also clears the screen
	if (!uboot)
		own_screen_start();

	interrupts_on();
	// The network over USB, in the background from here on (about 35 ms,
	// mostly the controller switching to device mode)
	klog("kernel: started at %lu ms, EL%u", boot_kernel / 24000, (uint32_t)current_el());
	net_start();
	log_start();
	usb_start();
	usb_irq_on();
	show_info(device_tree, start_ticks);
	buttons_init();
	buttons_irq_init();

	console_color(YELLOW);
	kputs("A keys B power X LCD Y I2C START EL3\nSELECT RAM R1 boot ROM L1 USB\n");
	uint32_t row = console_row();
	label("Running");
	console_goto(0, row + 1);
	label("IRQs");
	console_goto(0, row + 2);
	label("Late");
	console_goto(0, row + 3);
	label("Awake");
	console_goto(0, row + 4);
	label("Light");
	backlight_init();
	uint32_t light = backlight_get();
	show_light(row + 4, light, 0);

	// Did the timer interrupt arrive? Give it a fifth of a second.
	uint64_t t = now();
	while (!timer_ticks && now() - t < ticks_per_second / 5)
		;
	irqs_work = timer_ticks != 0;
	if (!irqs_work) {   // say why, then fall back to polling
		console_goto(8, row + 1);
		console_color(YELLOW);
		kprintf("none! pending %d, timer %lx", gic_pending(TIMER_IRQ),
			READ_SYSREG(cntp_ctl_el0));
	}

	// A clock driven by the timer interrupt. Between ticks the CPU sleeps
	// (wfi); we add up how long, to see how much of the time it's awake.
	boot_menu = now();
	klog("kernel: picture at %lu ms, menu at %lu ms", boot_picture / 24000, boot_menu / 24000);
	uint64_t asleep = 0, since = now(), shown = ~0UL;
	uint64_t last_press = now();
	uint32_t held = buttons_pressed();
	int dark = 0;
	for (;;) {
		uint64_t before_wfi = now();
		wait();
		asleep += now() - before_wfi;

		uint32_t pressed = buttons_pressed();
		uint32_t down = pressed & ~held;   // just pressed
		held = pressed;
		if (down) {
			last_press = now();
			if (dark) {   // the first press only switches the light back on
				dark = 0;
				backlight_set(light);
				show_light(row + 4, light, 0);
				down = 0;
			}
		}
		if (down & (bit_of("A") | bit_of("B") | bit_of("X") | bit_of("Y") | bit_of("START") | bit_of("SEL") | bit_of("R1") | bit_of("L1")))
			break;
		if (down & (bit_of("VOL+") | bit_of("VOL-"))) {
			if (down & bit_of("VOL+"))
				light = light >= 90 ? 100 : light + 10;
			else
				light = light <= 20 ? 10 : light - 10;   // not 0: you'd see nothing
			backlight_set(light);
			show_light(row + 4, light, 0);
		}
		if (!dark && now() - last_press > 30 * ticks_per_second) {
			dark = 1;
			backlight_set(0);
			show_light(row + 4, light, 1);
		}

		uint64_t ticks = timer_ticks;
		if (ticks == shown)
			continue;
		shown = ticks;

		uint64_t tenths = irqs_work ? ticks / 6 : (now() - start_ticks) * 10 / ticks_per_second;
		console_color(GREEN);
		console_goto(8, row);
		kprintf("%lu.%lu s   ", tenths / 10, tenths % 10);
		if (!irqs_work)
			continue;
		console_color(WHITE);
		console_goto(8, row + 1);
		kprintf("%lu timer interrupts", ticks);
		// counter ticks to nanoseconds: x 1000 / 24
		console_goto(8, row + 2);
		kprintf("min %lu avg %lu max %lu ns ", timer_late_min * 125 / 3,
			timer_late_sum * 125 / 3 / ticks, timer_late_max * 125 / 3);
		uint64_t total = now() - since;
		uint64_t awake = (total - asleep) * 1000 / total;   // tenths of a percent
		console_goto(8, row + 3);
		kprintf("%lu.%lu%% of the time   ", awake / 10, awake % 10);
	}
	uint32_t chosen = held;
	let_go();
	static const char *const names[] = { "A", "B", "X", "Y", "START", "SEL", "R1", "L1" };
	for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (chosen & bit_of(names[i])) {
			klog("menu: %s", names[i]);
			break;
		}
	if (chosen & bit_of("B"))
		power_page();
	else if (chosen & bit_of("X"))
		display_report();
	else if (chosen & bit_of("Y"))
		scan_page();
	else if (chosen & bit_of("START"))
		el3_page();
	else if (chosen & bit_of("SEL"))
		save_screen(ram_page, ram_save, "Saved to sector 12288");
	else if (chosen & bit_of("L1"))
		usb_screen();
	else if (chosen & bit_of("R1"))
		save_screen(rom_page, rom_save, "Saved to sector 12416");
	button_test();
}

// The compiler may turn loops into calls to these, and there's no C library.
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
	const uint8_t *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}
