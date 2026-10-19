// A tiny network stack on the USB link to the Mac.
//
// The link is one Ethernet cable with two ends: the Mac and us. Three
// protocols are enough to make `ping` work:
//
//   DHCP  the Mac asks "who can give me an address?" (that's the 342-byte
//         frame it kept sending). We answer: you're 192.168.36.2.
//   ARP   before sending to 192.168.36.1, the Mac asks "which Ethernet
//         address has that IP?" We say: ours.
//   ICMP  ping: an "echo request". We send the same bytes back as a reply.
//
// Everything on the wire is big-endian ("network order"); our CPU is
// little-endian, so 16- and 32-bit fields are put together byte by byte.

#include <stdint.h>
#include "console.h"
#include "log.h"
#include "loader.h"
#include "net.h"
#include "usb.h"

static const uint8_t our_ip[4] = { 192, 168, 36, 1 };
static const uint8_t mac_ip[4] = { 192, 168, 36, 2 };   // what DHCP gives the Mac
static const uint8_t mask[4] = { 255, 255, 255, 0 };

static struct {
	uint32_t arps, offers, acks, pings, other;
	uint8_t pinger[4];
} st;

static uint8_t out[1600] __attribute__((aligned(4)));

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void copy(uint8_t *to, const uint8_t *from, uint32_t n)
{
	while (n--)
		*to++ = *from++;
}
static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	while (n--)
		if (*a++ != *b++)
			return 0;
	return 1;
}

// The Internet checksum: add up 16-bit words, fold the carries back in,
// and flip the bits. A receiver sums the whole thing, checksum included,
// and expects 0xffff.
static uint32_t checksum(const uint8_t *p, uint32_t len)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i + 1 < len; i += 2)
		sum += get16(p + i);
	if (len & 1)
		sum += (uint32_t)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum & 0xffff;
}

// Ethernet header: to, from, type
static void ethernet(const uint8_t *to, uint32_t type)
{
	copy(out, to, 6);
	copy(out + 6, usb_mac_ours, 6);
	put16(out + 12, type);
}

// IPv4 header (20 bytes, at out + 14), for `len` bytes of payload
static void ipv4(const uint8_t *to, uint32_t protocol, uint32_t len)
{
	uint8_t *ip = out + 14;
	ip[0] = 0x45;            // version 4, header 5 words
	ip[1] = 0;
	put16(ip + 2, 20 + len);
	put16(ip + 4, 0);        // id
	put16(ip + 6, 0x4000);   // don't fragment
	ip[8] = 64;              // time to live
	ip[9] = (uint8_t)protocol;
	put16(ip + 10, 0);
	copy(ip + 12, our_ip, 4);
	copy(ip + 16, to, 4);
	put16(ip + 10, checksum(ip, 20));
}

// ---- ARP --------------------------------------------------------------------
static void arp(const uint8_t *f, uint32_t len)
{
	const uint8_t *a = f + 14;
	if (len < 42 || get16(a) != 1 || get16(a + 2) != 0x0800 || get16(a + 6) != 1)
		return;   // not an Ethernet/IPv4 request
	if (!same(a + 24, our_ip, 4))
		return;   // asking about someone else
	ethernet(a + 8, 0x0806);
	uint8_t *r = out + 14;
	put16(r, 1);
	put16(r + 2, 0x0800);
	r[4] = 6, r[5] = 4;
	put16(r + 6, 2);              // a reply
	copy(r + 8, usb_mac_ours, 6); // "I have it"
	copy(r + 14, our_ip, 4);
	copy(r + 18, a + 8, 6);       // to the asker
	copy(r + 24, a + 14, 4);
	if (!usb_send(out, 42))
		st.arps++;
}

// ---- ping -------------------------------------------------------------------
static void icmp(const uint8_t *f, uint32_t len, uint32_t ihl, uint32_t total)
{
	const uint8_t *m = f + 14 + ihl;
	if (m[0] != 8 || total < ihl + 8)
		return;   // only echo requests
	uint32_t n = total - ihl;   // the ICMP message: header and data
	ethernet(f + 6, 0x0800);
	ipv4(f + 14 + 12, 1, n);
	uint8_t *r = out + 14 + 20;
	copy(r, m, n);
	r[0] = 0;                 // echo reply
	put16(r + 2, 0);
	put16(r + 2, checksum(r, n));
	copy(st.pinger, f + 14 + 12, 4);
	if (!usb_send(out, 14 + 20 + n) && !st.pings++)
		klog("net: first ping answered, from %u.%u.%u.%u", st.pinger[0], st.pinger[1],
		     st.pinger[2], st.pinger[3]);
	(void)len;
}

// ---- DHCP -------------------------------------------------------------------
// The message (BOOTP's layout from 1985, plus "options" at the end):
//   0 op (1 ask, 2 answer)   4 transaction id   10 flags   16 "your" address
//   28 client hardware address   236 magic 63 82 53 63   240 options
static void dhcp(const uint8_t *f, uint32_t len, uint32_t ihl)
{
	const uint8_t *d = f + 14 + ihl + 8;
	if (len < 14 + ihl + 8 + 240 || d[0] != 1 || get16(f + 14 + ihl + 2) != 67 ||
	    d[236] != 0x63 || d[237] != 0x82 || d[238] != 0x53 || d[239] != 0x63)
		return;
	// Option 53: which kind of message
	uint32_t kind = 0;
	for (uint32_t i = 240; 14 + ihl + 8 + i + 1 < len && d[i] != 255;) {
		if (d[i] == 0) {   // padding
			i++;
			continue;
		}
		if (d[i] == 53)
			kind = d[i + 2];
		i += 2 + d[i + 1];
	}
	uint32_t answer;
	if (kind == 1)          // DISCOVER -> OFFER
		answer = 2;
	else if (kind == 3)     // REQUEST -> ACK
		answer = 5;
	else
		return;

	static const uint8_t everyone[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t everyone_ip[4] = { 255, 255, 255, 255 };
	uint8_t *r = out + 14 + 20 + 8;
	for (uint32_t i = 0; i < 300; i++)
		r[i] = 0;
	r[0] = 2;                       // an answer
	r[1] = 1, r[2] = 6;             // Ethernet, 6-byte addresses
	copy(r + 4, d + 4, 4);          // the same transaction
	copy(r + 10, d + 10, 2);
	copy(r + 16, mac_ip, 4);        // your address
	copy(r + 20, our_ip, 4);        // the server
	copy(r + 28, d + 28, 16);       // your hardware address
	r[236] = 0x63, r[237] = 0x82, r[238] = 0x53, r[239] = 0x63;
	uint8_t *o = r + 240;
	*o++ = 53, *o++ = 1, *o++ = (uint8_t)answer;
	*o++ = 54, *o++ = 4, copy(o, our_ip, 4), o += 4;              // from us
	*o++ = 51, *o++ = 4, *o++ = 0, *o++ = 1, *o++ = 0x51, *o++ = 0x80;   // a day
	*o++ = 1, *o++ = 4, copy(o, mask, 4), o += 4;                 // 192.168.36.x
	*o++ = 255;
	// No router and no DNS server on purpose: the Mac keeps using Wi-Fi
	// for everything else and only sends 192.168.36.x our way.

	// To everyone: the Mac has no address to be reached at yet
	uint32_t n = 300;   // BOOTP's traditional minimum
	ethernet(everyone, 0x0800);
	ipv4(everyone_ip, 17, 8 + n);
	uint8_t *u = out + 14 + 20;
	put16(u, 67);
	put16(u + 2, 68);
	put16(u + 4, 8 + n);
	put16(u + 6, 0);   // no UDP checksum (allowed on IPv4)
	if (!usb_send(out, 14 + 20 + 8 + n)) {
		if (answer == 2)
			st.offers++;
		else
			st.acks++;
		klog("net: DHCP %s 192.168.36.2", answer == 2 ? "offered" : "gave the Mac");
	}
}

// ---- UDP out ------------------------------------------------------------------
// To 255.255.255.255: whoever listens on `port` gets it, whatever address
// the Mac has (or before it has one from us)
int net_udp(uint32_t port, const void *data, uint32_t len)
{
	static const uint8_t everyone[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t everyone_ip[4] = { 255, 255, 255, 255 };
	if (len > sizeof out - 14 - 20 - 8)
		return -1;
	ethernet(everyone, 0x0800);
	ipv4(everyone_ip, 17, 8 + len);
	uint8_t *u = out + 14 + 20;
	put16(u, port);
	put16(u + 2, port);
	put16(u + 4, 8 + len);
	put16(u + 6, 0);
	copy(u + 8, data, len);
	return usb_send(out, 14 + 20 + 8 + len);
}

// ---- the loader: a new kernel from the Mac ------------------------------------
static void loader(const uint8_t *f, uint32_t ihl, uint32_t total)
{
	const uint8_t *u = f + 14 + ihl;
	uint32_t n = get16(u + 4);   // UDP length, header included
	if (n < 8 || n > total - ihl)
		return;
	int go;
	uint8_t *r = out + 14 + 20 + 8;
	uint32_t reply = loader_packet(u + 8, n - 8, r, &go);
	if (!reply)
		return;
	// Back to whoever sent it, from our port to theirs
	ethernet(f + 6, 0x0800);
	ipv4(f + 14 + 12, 17, 8 + reply);
	uint8_t *ru = out + 14 + 20;
	put16(ru, LOADER_PORT);
	put16(ru + 2, get16(u));
	put16(ru + 4, 8 + reply);
	put16(ru + 6, 0);
	usb_send(out, 14 + 20 + 8 + reply);
	if (go) {
		usb_drain(100000);   // the Mac has our "ok" before we vanish
		log_flush();         // and our last words
		usb_drain(100000);
		loader_start_new();
	}
}

// ---- every frame from the Mac ----------------------------------------------
static void frame_in(const uint8_t *f, uint32_t len)
{
	uint32_t type = get16(f + 12);
	if (type == 0x0806) {
		arp(f, len);
		return;
	}
	if (type != 0x0800 || len < 34 || f[14] >> 4 != 4) {
		st.other++;
		return;
	}
	uint32_t ihl = (f[14] & 15) * 4, total = get16(f + 16);
	if (ihl < 20 || 14 + total > len) {
		st.other++;
		return;
	}
	uint32_t protocol = f[23];
	if (protocol == 1 && same(f + 30, our_ip, 4))
		icmp(f, len, ihl, total);
	else if (protocol == 17 && same(f + 30, our_ip, 4) && total >= ihl + 8 &&
		 get16(f + 14 + ihl + 2) == LOADER_PORT)
		loader(f, ihl, total);
	else if (protocol == 17)
		dhcp(f, len, ihl);
	else
		st.other++;
}

void net_start(void)
{
	usb_frame_in = frame_in;
}

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

void net_lines(uint32_t row)
{
	console_goto(0, row);
	console_color(YELLOW);
	kputs("Network: we are 192.168.36.1\n");
	label("DHCP");
	kprintf("%u offers, %u acks   \n", st.offers, st.acks);
	label("ARP");
	kprintf("%u answered   \n", st.arps);
	label("Ping");
	if (st.pings)
		kprintf("%u answered, from %u.%u.%u.%u \n", st.pings, st.pinger[0], st.pinger[1],
			st.pinger[2], st.pinger[3]);
	else
		kputs("none yet\n");
	label("Ignored");
	kprintf("%u frames   \n", st.other);
}
