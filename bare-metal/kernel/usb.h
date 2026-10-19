#pragma once
// USB: the console as a network adapter for the Mac (CDC-ECM)
#include <stdint.h>

int usb_start(void);    // power, clocks, PHY, controller; then connect (once)
void usb_irq_on(void);  // from now on usb_poll runs on the USB interrupt
void usb_poll(void);    // handle whatever the controller has for us
int usb_link_up(void);  // the Mac has the network interface up
int usb_polling(void);  // inside usb_poll right now
extern void (*usb_idle)(void);   // called at the end of each usb_poll
void usb_page(void);    // what's happened so far (rows 0-13)
int usb_send(const uint8_t *frame, uint32_t len);   // a frame to the Mac; 0 = sent
void usb_drain(uint32_t us);   // wait (up to us) until the Mac has taken it
void usb_stop(void);           // off the bus, no more interrupts

extern const uint8_t usb_mac_ours[6];   // our end of the link

// A network frame arrived (from the Mac); set by whoever wants them
extern void (*usb_frame_in)(const uint8_t *frame, uint32_t len);
