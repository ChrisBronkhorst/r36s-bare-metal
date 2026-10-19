#pragma once
// A tiny network stack on the USB link: ARP, ping and a DHCP server
#include <stdint.h>

void net_start(void);          // take the frames the USB driver receives
void net_lines(uint32_t row);  // what it's done, for the USB page
int net_udp(uint32_t port, const void *data, uint32_t len);   // broadcast; 0 = sent
