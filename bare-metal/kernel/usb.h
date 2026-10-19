#pragma once
// USB: the console as a network adapter for the Mac (CDC-ECM)
#include <stdint.h>

int usb_start(void);    // power, clocks, PHY, controller; then connect
void usb_poll(void);    // handle whatever the controller has for us
void usb_page(void);    // what's happened so far

// A network frame arrived (from the Mac); set by whoever wants them
extern void (*usb_frame_in)(const uint8_t *frame, uint32_t len);
