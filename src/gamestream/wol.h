// Wake-on-LAN: the magic packet that brings a sleeping PC back.
#pragma once

#include <stdint.h>

#define WOL_PACKET_SIZE 102

/* Fill out from "AA:BB:CC:DD:EE:FF". Returns 0, or -1 for an invalid or all-zero MAC. */
int wol_build_packet(const char *mac, uint8_t out[WOL_PACKET_SIZE]);

/* Broadcast it on the local network; host, if a dotted IPv4, also targets its
 * /24 broadcast address. Returns how many sends succeeded. */
int wol_send(const char *mac, const char *host);
