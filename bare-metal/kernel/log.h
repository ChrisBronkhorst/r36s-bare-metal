#pragma once
// The kernel's log: timestamped lines, kept in memory and sent to the Mac
// over the USB network (UDP port 5000) whenever the link is up.
// On the Mac: nc -ul 5000
#include <stdint.h>

void klog(const char *fmt, ...);   // one line; the newline is added
void log_start(void);              // send from now on
void log_lines(uint32_t row);      // counters, for the USB page
