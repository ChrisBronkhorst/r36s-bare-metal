// USB: the console as a network adapter for the Mac.
//
// The RK3326's USB "OTG" port is a Synopsys DesignWare USB 2.0 controller
// (DWC2, at 0xff300000) behind an Innosilicon PHY (the analog side, set up
// through its own GRF at 0xff2c0000). OTG means it can be either end of the
// cable; we force it to be the device, and the Mac is the host.
//
// USB is host-driven: the device never speaks first. The Mac resets the
// bus, then asks questions on endpoint 0 ("control transfers"): who are you
// (descriptors), here's your address, use configuration 1... Our answers
// say we're a CDC-ECM device, the USB standard for an Ethernet adapter,
// which macOS drives with its own built-in driver. After that, network
// frames flow on two "bulk" endpoints: OUT (Mac -> us) and IN (us -> Mac).
//
// Like our SD driver, this one polls: no interrupts, no DMA. Everything
// goes through the controller's FIFOs. usb_poll() must run often (the Mac
// expects answers within a few milliseconds).

#include <stdint.h>
#include "console.h"
#include "sysreg.h"
#include "exception.h"
#include "log.h"
#include "usb.h"

#define OTG     0xff300000UL
#define PHYGRF  0xff2c0000UL   // the USB PHY's settings and status
#define CRU     0xff2b0000UL
#define PMUCRU  0xff2bc000UL
#define PMU     0xff000000UL

// ---- DWC2 registers ----------------------------------------------------------
#define GOTGCTL   0x000
#define GAHBCFG   0x008
#define GUSBCFG   0x00c
#define GRSTCTL   0x010
#define GINTSTS   0x014
#define GINTMSK   0x018
#define GRXSTSP   0x020   // pop the next "what arrived" entry
#define GRXFSIZ   0x024
#define GNPTXFSIZ 0x028   // endpoint 0's transmit FIFO
#define GSNPSID   0x040   // "OT" + version
#define GHWCFG2   0x048
#define GHWCFG3   0x04c
#define GHWCFG4   0x050
#define DPTXFSIZ(n) (0x104 + 4 * ((n) - 1))   // IN endpoint n's FIFO
#define DCFG      0x800
#define DCTL      0x804
#define DSTS      0x808
#define DIEPMSK   0x810
#define DOEPMSK   0x814
#define DAINT     0x818
#define DAINTMSK  0x81c
#define DIEPCTL(n)  (0x900 + 0x20 * (n))
#define DIEPINT(n)  (0x908 + 0x20 * (n))
#define DIEPTSIZ(n) (0x910 + 0x20 * (n))
#define DTXFSTS(n)  (0x918 + 0x20 * (n))
#define DOEPCTL(n)  (0xb00 + 0x20 * (n))
#define DOEPINT(n)  (0xb08 + 0x20 * (n))
#define DOEPTSIZ(n) (0xb10 + 0x20 * (n))
#define FIFO(n)     (0x1000 + 0x1000 * (n))   // reads pop OUT data, writes push IN

#define I_RXFLVL   (1U << 4)    // GINTSTS: something to pop from GRXSTSP
#define I_USBSUSP  (1U << 11)
#define I_USBRST   (1U << 12)
#define I_ENUMDONE (1U << 13)
#define I_IEPINT   (1U << 18)
#define I_OEPINT   (1U << 19)

#define EP_ENA    (1U << 31)
#define EP_DIS    (1U << 30)
#define EP_D0PID  (1U << 28)
#define EP_SNAK   (1U << 27)
#define EP_CNAK   (1U << 26)
#define EP_STALL  (1U << 21)
#define EP_BULK   (2U << 18)
#define EP_INTR   (3U << 18)
#define EP_ACTIVE (1U << 15)

// Our endpoints
// Our endpoints. The controller can be built with some endpoints one-way
// only (GHWCFG1 says which), so usb_start picks the numbers:
static uint32_t ep_in = 1;    // bulk IN: frames to the Mac
static uint32_t ep_out = 1;   // bulk OUT: frames from the Mac
static uint32_t ep_ntf = 2;   // interrupt IN: "the cable is plugged in"
// Each IN endpoint sends from its own transmit FIFO (endpoint 0 has FIFO 0)
#define TXF_DATA   1
#define TXF_NOTIFY 2

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static uint32_t r(uint32_t reg) { return rd(OTG + reg); }
static void w(uint32_t reg, uint32_t v) { wr(OTG + reg, v); }

static uint64_t now(void) { return READ_SYSREG(cntpct_el0); }
static void delay_us(uint64_t us)
{
	uint64_t end = now() + us * 24;
	while (now() < end)
		;
}
static int wait_for(uintptr_t a, uint32_t mask, uint32_t want, uint64_t us)
{
	uint64_t end = now() + us * 24;
	while ((rd(a) & mask) != want)
		if (now() > end)
			return -1;
	return 0;
}

void (*usb_frame_in)(const uint8_t *frame, uint32_t len);

// What's happened, for the page
static struct {
	int step;                  // how far usb_start got
	uint32_t id, hwcfg1, hwcfg2, hwcfg3, hwcfg4;
	uint32_t ntf_seen;         // every event the notify endpoint flagged
	uint32_t resets, enums, suspends, setups, stalls;
	uint32_t speed;            // 0 high, 1/3 full
	uint32_t address, config, alt;
	uint8_t last_setup[8];
	uint32_t frames_in, bytes_in, last_type, last_len, frames_too_big;
	uint32_t frames_out, bytes_out, send_drops;
	uint32_t notifies, notified;   // queued to the Mac, taken by it
	uint32_t descs, set_ifaces, clears, classes;
} st;

static uint32_t frames_at_link_up;   // frames_in when the link came up

// ---- descriptors --------------------------------------------------------------
// Little-endian 16-bit values, split into bytes
#define W(x) ((x) & 0xff), ((x) >> 8)

// pid.codes' test IDs: free for private testing, never for products
#define VID 0x1209
#define PID 0x0001

static const uint8_t device_desc[18] = {
	18, 1, W(0x0200),        // USB 2.0
	2, 0, 0,                 // class: communications (CDC)
	64,                      // endpoint 0 packets: 64 bytes
	W(VID), W(PID), W(0x0100),
	1, 2, 3,                 // strings: maker, product, serial
	1,                       // one configuration
};

// The same, as it would be at the other speed ("qualifier")
static const uint8_t qualifier_desc[10] = {
	10, 6, W(0x0200), 2, 0, 0, 64, 1, 0,
};

#define CONFIG_LEN (9 + 9 + 5 + 5 + 13 + 7 + 9 + 9 + 7 + 7)
static uint8_t config_desc[CONFIG_LEN] = {
	9, 2, W(CONFIG_LEN), 2, 1, 0, 0xc0, 50,   // 2 interfaces, self-powered, 100 mA

	// Interface 0: control. CDC, subclass 6 = Ethernet (ECM)
	9, 4, 0, 0, 1, 2, 6, 0, 0,
	5, 0x24, 0x00, W(0x0110),                 // CDC header: version 1.10
	5, 0x24, 0x06, 0, 1,                      // union: interface 0 controls 1
	13, 0x24, 0x0f, 4,                        // Ethernet: MAC address = string 4
	0, 0, 0, 0,                               //   no statistics
	W(1514), W(0), 0,                         //   largest frame, no filters
	7, 5, 0x82, 3, W(16), 8,      // interrupt IN, 16 bytes, every 16 ms

	// Interface 1: data. Setting 0 has no endpoints (link down); setting 1
	// has the two bulk ones (link up)
	9, 4, 1, 0, 0, 0x0a, 0, 0, 0,
	9, 4, 1, 1, 2, 0x0a, 0, 0, 0,
	7, 5, 0x81, 2, W(512), 0,       // bulk IN (patched to 64 at full speed)
	7, 5, 0x01, 2, W(512), 0,               // bulk OUT
};
// Where the endpoint numbers and packet sizes sit, for patching
#define NOTIFY_ADDR  (CONFIG_LEN - 39 + 2)
#define BULK_IN_ADDR (CONFIG_LEN - 14 + 2)
#define BULK_OUT_ADDR (CONFIG_LEN - 7 + 2)
#define BULK_IN_MPS  (CONFIG_LEN - 14 + 4)
#define BULK_OUT_MPS (CONFIG_LEN - 7 + 4)

// Locally administered addresses (bit 1 of the first byte): the Mac's end
// of the link (which ECM asks us to name) and ours
const uint8_t usb_mac_host[6] = { 0x02, 0x52, 0x33, 0x36, 0x53, 0x01 };
const uint8_t usb_mac_ours[6] = { 0x02, 0x52, 0x33, 0x36, 0x53, 0x02 };

static const char *const strings[] = {
	0, "Bare metal", "R36S network", "0001", 0 /* MAC, made below */,
};

static uint8_t string_buf[2 + 2 * 32];

// String descriptors are UTF-16; ours are ASCII, so every other byte is 0
static uint32_t string_desc(uint32_t i)
{
	char mac[13];
	const char *s;
	if (i == 0) {   // the list of languages: just US English
		string_buf[0] = 4, string_buf[1] = 3, string_buf[2] = 0x09, string_buf[3] = 0x04;
		return 4;
	}
	if (i == 4) {
		const char *hex = "0123456789ABCDEF";
		for (int k = 0; k < 6; k++) {
			mac[2 * k] = hex[usb_mac_host[k] >> 4];
			mac[2 * k + 1] = hex[usb_mac_host[k] & 15];
		}
		mac[12] = 0;
		s = mac;
	} else if (i < sizeof strings / sizeof strings[0]) {
		s = strings[i];
	} else {
		return 0;
	}
	uint32_t n = 0;
	for (; s[n] && n < 32; n++)
		string_buf[2 + 2 * n] = (uint8_t)s[n], string_buf[3 + 2 * n] = 0;
	string_buf[0] = (uint8_t)(2 + 2 * n);
	string_buf[1] = 3;
	return string_buf[0];
}

// ---- endpoint 0 -----------------------------------------------------------------
static uint32_t setup[2];         // the 8-byte request
static const uint8_t *ep0_data;   // what's left of our answer
static uint32_t ep0_left;
static int ep0_zlp;               // end the answer with an empty packet
static uint8_t ep0_small[4];

// Ready for the next request (up to 3 back-to-back SETUPs, or 64 bytes)
static void ep0_out_arm(void)
{
	w(DOEPTSIZ(0), 3U << 29 | 1U << 19 | 64);
	w(DOEPCTL(0), EP_ENA | EP_CNAK);
}

// Push `len` bytes into IN endpoint n's FIFO, a word at a time
static void fifo_write(uint32_t n, const uint8_t *p, uint32_t len)
{
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t v = 0;
		for (uint32_t k = 0; k < 4 && i + k < len; k++)
			v |= (uint32_t)p[i + k] << (8 * k);
		w(FIFO(n), v);
	}
}

// The next packet of our answer (64 bytes at most)
static void ep0_next(void)
{
	uint32_t n = ep0_left > 64 ? 64 : ep0_left;
	w(DIEPTSIZ(0), 1U << 19 | n);
	w(DIEPCTL(0), (r(DIEPCTL(0)) & 3) | EP_ENA | EP_CNAK);
	fifo_write(0, ep0_data, n);
	ep0_data += n;
	ep0_left -= n;
	if (n < 64)
		ep0_zlp = 0;   // a short packet already says "that's all"
}

static void ep0_send(const void *data, uint32_t len)
{
	uint32_t asked = setup[1] >> 16;   // wLength
	if (len > asked)
		len = asked;
	ep0_data = data;
	ep0_left = len;
	ep0_zlp = len < asked && len % 64 == 0;   // ended early, on a packet boundary
	ep0_next();
}

static void ep0_ack(void)   // the status stage: an empty IN packet
{
	ep0_left = 0;
	ep0_zlp = 0;
	w(DIEPTSIZ(0), 1U << 19 | 0);
	w(DIEPCTL(0), (r(DIEPCTL(0)) & 3) | EP_ENA | EP_CNAK);
}

static void ep0_stall(void)   // "I don't do that"
{
	st.stalls++;
	w(DIEPCTL(0), r(DIEPCTL(0)) | EP_STALL);
	w(DOEPCTL(0), r(DOEPCTL(0)) | EP_STALL);
}

// ---- the data endpoints -----------------------------------------------------------
static uint8_t frame_buf[2048] __attribute__((aligned(4)));
static uint32_t frame_len;
static uint32_t bulk_mps = 512;

static void data_out_arm(void)
{
	frame_len = 0;
	uint32_t packets = sizeof frame_buf / bulk_mps;
	w(DOEPTSIZ(ep_out), packets << 19 | sizeof frame_buf);
	w(DOEPCTL(ep_out), r(DOEPCTL(ep_out)) | EP_ENA | EP_CNAK);
}

static void endpoints_on(void)
{
	w(DIEPCTL(ep_in), EP_SNAK | EP_D0PID | TXF_DATA << 22 | EP_BULK | EP_ACTIVE | bulk_mps);
	w(DOEPCTL(ep_out), EP_SNAK | EP_D0PID | EP_BULK | EP_ACTIVE | bulk_mps);
	data_out_arm();
}

// Send one network frame to the Mac on the bulk IN endpoint. The frame ends
// with a packet shorter than the maximum; one that fits exactly would need
// an extra empty packet, so we add a byte instead (the IP length inside
// says where the data really ends). Waits while the FIFO drains. 0 = sent.
static void ep_in_abort(uint32_t n, uint32_t fifo);

int usb_send(const uint8_t *frame, uint32_t len)
{
	if (!st.alt || len > 1600) {
		st.send_drops++;
		return -1;
	}
	if (len < 60)
		len = 60;   // Ethernet's minimum; the caller's buffer must have room
	if (len % bulk_mps == 0)
		len++;
	// The last frame must be gone first; if the Mac never took it, give up
	// on it (it's stuck in the FIFO and would block everything after)
	if (wait_for(OTG + DIEPCTL(ep_in), EP_ENA, 0, 5000))
		ep_in_abort(ep_in, TXF_DATA);
	uint32_t packets = (len + bulk_mps - 1) / bulk_mps;
	w(DIEPTSIZ(ep_in), 1U << 29 | packets << 19 | len);
	w(DIEPCTL(ep_in), r(DIEPCTL(ep_in)) | EP_ENA | EP_CNAK);
	// A packet at a time, each when the FIFO has room for it
	for (uint32_t done = 0; done < len; done += bulk_mps) {
		uint32_t n = len - done < bulk_mps ? len - done : bulk_mps;
		uint64_t end = now() + 5000 * 24;
		while ((r(DTXFSTS(ep_in)) & 0xffff) < (n + 3) / 4)
			if (now() > end) {   // the Mac stopped reading: drop the frame
				ep_in_abort(ep_in, TXF_DATA);
				st.send_drops++;
				return -1;
			}
		fifo_write(ep_in, frame + done, n);
	}
	st.frames_out++;
	st.bytes_out += len;
	return 0;
}

static void endpoints_off(void)
{
	w(DIEPCTL(ep_in), 0);
	w(DOEPCTL(ep_out), 0);
}

// Stop an IN endpoint's pending transfer and empty its FIFO: ask it to
// NAK, wait until it does, disable it, wait, then flush
static void ep_in_abort(uint32_t n, uint32_t fifo)
{
	if (r(DIEPCTL(n)) & EP_ENA) {
		w(DIEPCTL(n), r(DIEPCTL(n)) | EP_SNAK);
		wait_for(OTG + DIEPINT(n), 1U << 6, 1U << 6, 1000);   // NAK in effect
		w(DIEPCTL(n), r(DIEPCTL(n)) | EP_DIS | EP_SNAK);
		wait_for(OTG + DIEPINT(n), 1U << 1, 1U << 1, 1000);   // disabled
		if (n == ep_ntf)
			st.ntf_seen |= r(DIEPINT(n));
		w(DIEPINT(n), 1U << 6 | 1U << 1);
	}
	w(GRSTCTL, fifo << 6 | 1U << 5);
	wait_for(OTG + GRSTCTL, 1U << 5, 0, 1000);
}

// Tell the Mac the "cable" is plugged in: NETWORK_CONNECTION (value 1),
// then CONNECTION_SPEED_CHANGE (480 Mbit/s each way). One at a time: the
// next goes when the Mac has taken the last.
static uint32_t notify_queue;   // bit 0: connection, bit 1: speed
static int notify_busy;

static void notify_kick(void)
{
	static const uint8_t connected[8] = { 0xa1, 0x00, W(1), W(0), W(0) };
	static const uint8_t speed[16] = { 0xa1, 0x2a, W(0), W(0), W(8),
		W(0x3800), W(0x1c9c), W(0x3800), W(0x1c9c) };   // 480,000,000
	if (notify_busy || !notify_queue)
		return;
	const uint8_t *msg = notify_queue & 1 ? connected : speed;
	uint32_t len = notify_queue & 1 ? 8 : 16;
	notify_queue &= notify_queue & 1 ? ~1U : ~2U;
	// Bits 29-30, "multi count": packets per microframe. 0 is invalid on
	// every IN endpoint but endpoint 0; the Mac's reads failed until it was 1
	w(DIEPTSIZ(ep_ntf), 1U << 29 | 1U << 19 | len);
	w(DIEPCTL(ep_ntf), r(DIEPCTL(ep_ntf)) | EP_ENA | EP_CNAK);
	fifo_write(ep_ntf, msg, len);   // pushed by endpoint number
	notify_busy = 1;
	st.notifies++;
}

static void notify_restart(void)
{
	ep_in_abort(ep_ntf, TXF_NOTIFY);
	notify_busy = 0;
	notify_queue = st.alt ? 3 : 0;
	notify_kick();
}

// ---- requests ------------------------------------------------------------------------
static void request(void)
{
	uint8_t type = setup[0] & 0xff, req = setup[0] >> 8 & 0xff;
	uint16_t value = setup[0] >> 16, index = setup[1] & 0xffff;
	st.setups++;
	for (int i = 0; i < 8; i++)
		st.last_setup[i] = (uint8_t)(setup[i / 4] >> (8 * (i % 4)));

	if ((type & 0x60) == 0x20) {   // CDC class requests
		st.classes++;
		if (req == 0x43)           // SET_ETHERNET_PACKET_FILTER: fine
			ep0_ack();
		else
			ep0_stall();
		return;
	}
	if (type & 0x60) {
		ep0_stall();
		return;
	}
	switch (req) {
	case 6: {   // GET_DESCRIPTOR
		uint32_t kind = value >> 8, i = value & 0xff;
		st.descs++;
		if (kind == 1)
			ep0_send(device_desc, sizeof device_desc);
		else if (kind == 2)
			ep0_send(config_desc, sizeof config_desc);
		else if (kind == 3 && string_desc(i))
			ep0_send(string_buf, string_buf[0]);
		else if (kind == 6)
			ep0_send(qualifier_desc, sizeof qualifier_desc);
		else
			ep0_stall();
		break;
	}
	case 5:     // SET_ADDRESS: this controller takes it before the status stage
		st.address = value & 0x7f;
		klog("usb: address %u", st.address);
		w(DCFG, (r(DCFG) & ~(0x7fU << 4)) | st.address << 4);
		ep0_ack();
		break;
	case 9:     // SET_CONFIGURATION
		st.config = value & 0xff;
		st.alt = 0;
		endpoints_off();
		ep_in_abort(ep_ntf, TXF_NOTIFY);
		notify_busy = 0;
		notify_queue = 0;
		if (st.config)
			w(DIEPCTL(ep_ntf), EP_SNAK | EP_D0PID | TXF_NOTIFY << 22 | EP_INTR |
			  EP_ACTIVE | 16);
		ep0_ack();
		break;
	case 8:     // GET_CONFIGURATION
		ep0_small[0] = (uint8_t)st.config;
		ep0_send(ep0_small, 1);
		break;
	case 11:    // SET_INTERFACE: data interface setting 1 = link up
		st.set_ifaces++;
		if (index == 1) {
			if (value && !st.alt) {
				frames_at_link_up = st.frames_in;
				endpoints_on();
				klog("usb: link up");
			} else if (!value && st.alt) {
				endpoints_off();
				klog("usb: link down");
			}
			st.alt = value;
			notify_restart();
		}
		ep0_ack();
		break;
	case 10:    // GET_INTERFACE
		ep0_small[0] = index == 1 ? (uint8_t)st.alt : 0;
		ep0_send(ep0_small, 1);
		break;
	case 0:     // GET_STATUS: nothing to report
		ep0_small[0] = ep0_small[1] = 0;
		ep0_send(ep0_small, 2);
		break;
	case 1:     // CLEAR_FEATURE: on an endpoint, "un-halt" it, which also
		        // restarts its packet sequence at DATA0
		st.clears++;
		if ((type & 0x1f) == 2 && value == 0) {
			uint32_t n = index & 0xf;
			if (index & 0x80) {
				w(DIEPCTL(n), (r(DIEPCTL(n)) & ~EP_STALL) | EP_D0PID);
				if (n == ep_ntf)
					notify_restart();
			} else if (n) {
				w(DOEPCTL(n), (r(DOEPCTL(n)) & ~EP_STALL) | EP_D0PID);
			}
		}
		ep0_ack();
		break;
	case 3:     // SET_FEATURE
		ep0_ack();
		break;
	default:
		ep0_stall();
	}
}

// ---- bus events -----------------------------------------------------------------------
static void on_reset(void)
{
	st.resets++;
	klog("usb: bus reset");
	st.address = st.config = st.alt = 0;
	w(DCFG, r(DCFG) & ~(0x7fU << 4));
	endpoints_off();
	w(DIEPCTL(ep_ntf), 0);
	notify_busy = 0;
	notify_queue = 0;
	w(GRSTCTL, 0x10U << 6 | 1U << 5);   // flush every transmit FIFO
	wait_for(OTG + GRSTCTL, 1U << 5, 0, 1000);
	ep0_out_arm();
}

static void on_enum_done(void)
{
	st.enums++;
	st.speed = r(DSTS) >> 1 & 3;
	klog("usb: %s speed", st.speed == 0 ? "high (480 Mbit/s)" : "full (12 Mbit/s)");
	bulk_mps = st.speed == 0 ? 512 : 64;   // high speed : full speed
	config_desc[BULK_IN_MPS] = config_desc[BULK_OUT_MPS] = (uint8_t)(bulk_mps & 0xff);
	config_desc[BULK_IN_MPS + 1] = config_desc[BULK_OUT_MPS + 1] = (uint8_t)(bulk_mps >> 8);
	w(DIEPCTL(0), 0);          // endpoint 0: 64-byte packets
	w(DCTL, r(DCTL) | 1U << 8);   // clear the global IN NAK
	ep0_out_arm();
}

// One entry from the receive FIFO: what arrived, on which endpoint, how big
static void receive(void)
{
	uint32_t s = r(GRXSTSP);
	uint32_t ep = s & 0xf, bytes = s >> 4 & 0x7ff, kind = s >> 17 & 0xf;
	uint32_t words = (bytes + 3) / 4;
	if (kind == 6) {                         // SETUP data: the 8-byte request
		for (uint32_t i = 0; i < words; i++) {
			uint32_t v = r(FIFO(0));
			if (i < 2)
				setup[i] = v;
		}
	} else if (kind == 2 && ep == ep_out)  { // part of a network frame
		for (uint32_t i = 0; i < words; i++) {
			uint32_t v = r(FIFO(0));
			if (frame_len + 4 * i + 4 <= sizeof frame_buf)
				*(uint32_t *)(frame_buf + frame_len + 4 * i) = v;
		}
		frame_len += bytes;
	} else {                                 // anything else: drain it
		for (uint32_t i = 0; i < words; i++)
			(void)r(FIFO(0));
	}
}

static void frame_done(void)
{
	if (frame_len > 1514) {
		st.frames_too_big++;
	} else if (frame_len >= 14) {
		st.frames_in++;
		st.bytes_in += frame_len;
		st.last_len = frame_len;
		st.last_type = (uint32_t)frame_buf[12] << 8 | frame_buf[13];
		if (usb_frame_in)
			usb_frame_in(frame_buf, frame_len);
	}
	data_out_arm();
}

void (*usb_idle)(void);
static int in_poll;

// Up, and the Mac has sent us something on it: the Mac drops what we send
// in the moment between switching the link on and its side being ready
int usb_link_up(void) { return st.alt && st.frames_in != frames_at_link_up; }
int usb_polling(void) { return in_poll; }

static void poll(void);

// From the USB interrupt, or from a loop: either way with IRQs masked, so
// the two never run at once
void usb_poll(void)
{
	uint64_t daif;
	__asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) : : "memory");
	if (st.step == 6 && !in_poll) {
		in_poll = 1;
		poll();
		in_poll = 0;
		if (usb_idle)
			usb_idle();   // e.g. send the log, now the link may be up
	}
	__asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

static void usb_interrupt(uint32_t id)
{
	(void)id;
	usb_poll();
}

// From now on the controller raises its interrupt line (GIC SPI 62) whenever
// GINTSTS has something we unmasked, and usb_poll runs then
void usb_irq_on(void)
{
	if (st.step != 6)
		return;
	irq_register(62 + 32, usb_interrupt, 0xa0);
	w(GAHBCFG, r(GAHBCFG) | 1);   // the global interrupt enable
}

static void poll(void)
{
	uint32_t g = r(GINTSTS);
	if (g & I_USBRST) {
		w(GINTSTS, I_USBRST);
		on_reset();
	}
	if (g & I_ENUMDONE) {
		w(GINTSTS, I_ENUMDONE);
		on_enum_done();
	}
	if (g & I_USBSUSP) {
		w(GINTSTS, I_USBSUSP);
		st.suspends++;
	}
	while (r(GINTSTS) & I_RXFLVL)
		receive();

	uint32_t daint = r(DAINT);
	if (daint & (1U << 16)) {                // endpoint 0 OUT
		uint32_t i = r(DOEPINT(0));
		w(DOEPINT(0), i);
		if (i & (1U << 3)) {                 // a request has arrived
			ep0_out_arm();
			request();
		} else if (i & 1) {                  // status stage done (or data)
			ep0_out_arm();
		}
	}
	if (daint & 1) {                         // endpoint 0 IN: a packet went out
		uint32_t i = r(DIEPINT(0));
		w(DIEPINT(0), i);
		if (i & 1) {
			if (ep0_left)
				ep0_next();
			else if (ep0_zlp) {
				ep0_zlp = 0;
				ep0_ack();
			}
		}
	}
	if (daint & (1U << (16 + ep_out))) {     // a whole frame from the Mac
		uint32_t i = r(DOEPINT(ep_out));
		w(DOEPINT(ep_out), i);
		if (i & 1)
			frame_done();
	}
	st.ntf_seen |= r(DIEPINT(ep_ntf));
	if (daint & (1U << ep_ntf)) {            // the Mac took a notification
		uint32_t i = r(DIEPINT(ep_ntf));
		w(DIEPINT(ep_ntf), i);
		if (i & 1) {
			st.notified++;
			notify_busy = 0;
			notify_kick();
		}
	}
	if (daint & (1U << ep_in))
		w(DIEPINT(ep_in), r(DIEPINT(ep_in)));
}

// ---- start ------------------------------------------------------------------------------
static void pmu_clear(uint32_t reg, uint32_t bit)
{
	if (!(rd(PMU + reg) & bit))
		return;
	wr(PMU + reg, rd(PMU + reg) & ~bit);
	if (rd(PMU + reg) & bit)
		wr(PMU + reg, bit << 16);   // the hiword-mask style, if it wants that
}

static int start(void);

// 0 = connected to the bus (the Mac decides the rest); otherwise the step
// that failed
int usb_start(void)
{
	static int result = -1;
	if (result >= 0)   // once is enough
		return result;
	result = start();
	if (result)
		klog("usb: failed at step %u", (uint32_t)result);
	else
		klog("usb: on the bus, waiting for the Mac");
	return result;
}

static int start(void)
{
	st.step = 1;
	// 1. The USB power domain (PMU: power bit 5, idle request bit 10)
	pmu_clear(0x18, 1U << 5);
	if (wait_for(PMU + 0x20, 1U << 5, 0, 100000))
		return 1;
	pmu_clear(0x64, 1U << 10);
	if (wait_for(PMU + 0x6c, 1U << 10 | 1U << 26, 0, 10000))
		return 1;

	st.step = 2;
	// 2. Clocks: the USB bus clocks (CRU gate 7, bits 2-5), the PHY GRF's
	// register clock (gate 16 bit 6), the PHY's 24 MHz reference (PMUCRU
	// gate 1 bit 9, from the crystal: clksel 2 bit 6 = 0)
	wr(CRU + 0x200 + 4 * 7, 0x3cU << 16);
	wr(CRU + 0x200 + 4 * 16, 0x40U << 16);
	wr(PMUCRU + 0x40 + 4 * 2, 0x40U << 16);
	wr(PMUCRU + 0x80 + 4 * 1, 0x200U << 16);
	// Out of reset: the controller (soft reset 66, 67) and the PHY (74-77)
	wr(CRU + 0x300 + 4 * 4, 0x3c0cU << 16 | 0x000cU);   // controller in reset
	delay_us(20);
	wr(CRU + 0x300 + 4 * 4, 0x3c0cU << 16);             // all out
	delay_us(100);

	st.step = 3;
	// 3. The PHY: not suspended (the controller drives it), 480 MHz clock out
	wr(PHYGRF + 0x100, 0xffffU << 16 | 0);
	wr(PHYGRF + 0x108, 0x10U << 16);
	delay_us(2000);   // its PLL locks and the UTMI clock starts

	st.step = 4;
	// 4. The controller: is it there? Then reset its core
	st.id = r(GSNPSID);
	st.hwcfg1 = r(0x044);   // GHWCFG1: 2 bits per endpoint, 1 = IN only, 2 = OUT only
	st.hwcfg2 = r(GHWCFG2);
	st.hwcfg3 = r(GHWCFG3);
	st.hwcfg4 = r(GHWCFG4);
	if ((st.id >> 16) != 0x4f54)   // "OT"
		return 4;
	// Pick endpoints that can go the way we need
	uint32_t eps = st.hwcfg2 >> 10 & 0xf;   // how many, besides 0
	ep_in = ep_out = ep_ntf = 0;
	for (uint32_t i = 1; i <= eps; i++) {
		uint32_t dir = st.hwcfg1 >> (2 * i) & 3;
		if (!(dir & 1) && !ep_out)
			ep_out = i;
		if (!(dir & 2) && !ep_in)
			ep_in = i;
		else if (!(dir & 2) && !ep_ntf)
			ep_ntf = i;
	}
	if (!ep_in || !ep_out || !ep_ntf)
		return 4;
	config_desc[NOTIFY_ADDR] = (uint8_t)(0x80 | ep_ntf);
	config_desc[BULK_IN_ADDR] = (uint8_t)(0x80 | ep_in);
	config_desc[BULK_OUT_ADDR] = (uint8_t)ep_out;
	if (wait_for(OTG + GRSTCTL, 1U << 31, 1U << 31, 100000))
		return 4;
	w(GRSTCTL, 1);
	// Newer cores say "done" in bit 29; older ones just clear bit 0
	uint64_t end = now() + 100000 * 24;
	for (;;) {
		uint32_t v = r(GRSTCTL);
		if (v & (1U << 29)) {
			w(GRSTCTL, (v & ~1U) | 1U << 29);
			break;
		}
		if (!(v & 1))
			break;
		if (now() > end)
			return 4;
	}
	if (wait_for(OTG + GRSTCTL, 1U << 31, 1U << 31, 100000))
		return 4;

	st.step = 5;
	// 5. Device mode, the PHY's data width, "the cable has power" (we can't
	// see the Mac's 5 V ourselves, so say so), FIFO sizes
	uint32_t width16 = (st.hwcfg4 >> 14 & 3) != 0;
	uint32_t cfg = r(GUSBCFG) & ~(1U << 29 | 0xfU << 10 | 1U << 3 | 1U << 4 | 1U << 6 | 7U);
	cfg |= 1U << 30 | (width16 ? 1U << 3 | 5U << 10 : 9U << 10) | 7U;
	w(GUSBCFG, cfg);
	delay_us(30000);    // switching modes takes up to 25 ms
	w(GOTGCTL, r(GOTGCTL) | 1U << 7 | 1U << 6 | 1U << 3 | 1U << 2);
	w(GAHBCFG, 0);      // no DMA, no interrupt line: we poll

	w(DCTL, r(DCTL) | 1U << 1);   // stay off the bus while we set up
	w(DCFG, 1U << 2);             // high speed; ignore stray status packets
	// FIFO RAM (in 4-byte words): receive 280, then a transmit FIFO for
	// endpoint 0 (16 = 64 bytes), bulk IN (128 = 512 bytes), notify (16)
	w(GRXFSIZ, 280);
	w(GNPTXFSIZ, 16U << 16 | 280);
	w(DPTXFSIZ(TXF_DATA), 128U << 16 | 296);
	w(DPTXFSIZ(TXF_NOTIFY), 16U << 16 | 424);
	w(GRSTCTL, 0x10U << 6 | 1U << 5);   // flush all transmit FIFOs
	wait_for(OTG + GRSTCTL, 1U << 5, 0, 10000);
	w(GRSTCTL, 1U << 4);                // and the receive FIFO
	wait_for(OTG + GRSTCTL, 1U << 4, 0, 10000);

	// The endpoint events we act on: transfer done, endpoint disabled,
	// SETUP done (OUT) / timeout (IN). Not "IN token with nothing to send",
	// which the Mac's idle polling would raise all the time
	w(DIEPMSK, 1 | 2 | 8);
	w(DOEPMSK, 1 | 2 | 8);
	w(DAINTMSK, 0xffffffff);
	w(GINTMSK, I_RXFLVL | I_USBSUSP | I_USBRST | I_ENUMDONE | I_IEPINT | I_OEPINT);
	w(GINTSTS, 0xffffffff);

	st.step = 6;
	// 6. Connect: the controller pulls D+ up, and the Mac sees a device
	w(DCTL, r(DCTL) & ~(1U << 1));
	return 0;
}

// ---- the page ---------------------------------------------------------------------------
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

static const char *type_name(uint32_t t)
{
	switch (t) {
	case 0x0800: return "IPv4";
	case 0x0806: return "ARP";
	case 0x86dd: return "IPv6";
	}
	return "other";
}

void usb_page(void)
{
	static const char *steps[] = { "", "power", "clocks", "PHY", "controller", "setup", "on the bus" };
	console_goto(0, 0);
	console_color(YELLOW);
	kputs("USB: a network adapter\n");
	label("Started");
	console_color(st.step == 6 ? GREEN : ALERT);
	kprintf("%s       \n", st.step == 6 ? "on the bus" : steps[st.step]);
	label("Core");
	kprintf("%08x, FIFO %u words\n", st.id, st.hwcfg3 >> 16);
	uint32_t phy = rd(PHYGRF + 0x120);
	label("Cable");
	kprintf("%s, lines %u   \n", phy & (1U << 9) ? "5 V there" : "no 5 V", phy >> 4 & 3);
	label("Bus");
	kprintf("%u resets, %u suspends   \n", st.resets, st.suspends);
	label("Speed");
	if (st.enums)
		kprintf("%s (480 or 12 Mbit/s)\n", st.speed == 0 ? "high" : "full");
	else
		kputs("not yet\n");
	label("Address");
	kprintf("%u, config %u, link %s   \n", st.address, st.config, st.alt ? "up" : "down");
	label("Requests");
	kprintf("%u, %u refused   \n", st.setups, st.stalls);
	label("");
	kprintf("desc %u alt %u clr %u cdc %u \n", st.descs, st.set_ifaces, st.clears, st.classes);
	label("Notified");
	kprintf("%u sent, %u taken   \n", st.notifies, st.notified);
	label("Last");
	for (int i = 0; i < 8; i++)
		kprintf("%02x", st.last_setup[i]);
	kputc('\n');
	label("In/out/nt");
	kprintf("%u/%u/%u, cfg1 %08x\n", ep_in, ep_out, ep_ntf, st.hwcfg1);
	label("Frames");
	kprintf("%u in, %u out, %u lost  \n", st.frames_in, st.frames_out, st.send_drops);
	label("Last one");
	if (st.frames_in)
		kprintf("%s (%04x), %u bytes   \n", type_name(st.last_type), st.last_type, st.last_len);
	else
		kputs("-\n");
}
