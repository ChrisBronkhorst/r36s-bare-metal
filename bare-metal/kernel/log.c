// The kernel's log. klog() formats a line into a ring buffer (16 KB, the
// oldest lines go first when it's full) and returns quickly. The lines go
// to the Mac as UDP broadcasts on port 5000 when the link is up: right away
// if we can, otherwise at the end of the next USB interrupt. So lines from
// before the cable was plugged in arrive too, as long as they still fit.

#include <stdarg.h>
#include <stdint.h>
#include "console.h"
#include "log.h"
#include "net.h"
#include "sysreg.h"
#include "usb.h"

#define LOG_SIZE 16384
static char ring[LOG_SIZE];
static uint32_t head, tail;   // count up forever; ring[i % LOG_SIZE]
static uint32_t lines, sent_lines, dropped_bytes;
static int started;

static void put(char c)
{
	if (head - tail == LOG_SIZE) {   // full: forget the oldest byte
		tail++;
		dropped_bytes++;
	}
	ring[head++ % LOG_SIZE] = c;
}

static void put_formatted(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	kformat(put, fmt, args);
	va_end(args);
}

static uint64_t mask_irqs(void)
{
	uint64_t daif;
	__asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) : : "memory");
	return daif;
}
static void restore_irqs(uint64_t daif) { __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory"); }

// Send what we have, in packets of whole lines (up to 1400 bytes each)
static void flush(void)
{
	static char packet[1400];
	if (!started || !usb_link_up())
		return;
	while (tail != head) {
		uint32_t n = head - tail < sizeof packet ? head - tail : sizeof packet;
		// End at the last complete line, if there's one
		uint32_t end = n;
		while (end && ring[(tail + end - 1) % LOG_SIZE] != '\n')
			end--;
		if (end)
			n = end;
		uint32_t newlines = 0;
		for (uint32_t i = 0; i < n; i++) {
			packet[i] = ring[(tail + i) % LOG_SIZE];
			newlines += packet[i] == '\n';
		}
		if (net_udp(5000, packet, n))
			return;   // try again later
		tail += n;
		sent_lines += newlines;
	}
}

void klog(const char *fmt, ...)
{
	uint64_t daif = mask_irqs();
	uint64_t ms = READ_SYSREG(cntpct_el0) / 24000;   // since power-on
	put_formatted("[%4lu.%03lu] ", ms / 1000, ms % 1000);
	va_list args;
	va_start(args, fmt);
	kformat(put, fmt, args);
	va_end(args);
	put('\n');
	lines++;
	// Inside the USB code, sending now could tangle with what it's doing;
	// it flushes when it's done instead
	if (!usb_polling())
		flush();
	restore_irqs(daif);
}

void log_start(void)
{
	started = 1;
	usb_idle = flush;
}

void log_lines(uint32_t row)
{
	console_goto(0, row);
	console_color(GREY);
	kputs("Log      ");
	console_color(WHITE);
	kprintf("%u lines, %u sent, %u B lost \n", lines, sent_lines, dropped_bytes);
}
