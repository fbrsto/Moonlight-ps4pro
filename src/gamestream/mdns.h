// Host discovery: mDNS queries for _nvstream._tcp.local on the local network.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MDNS_MAX_HOSTS 8

typedef struct {
    char name[64];
    char addr[16]; /* dotted IPv4 */
    uint64_t last_seen_us; /* set when the host list records a reply */
} mdns_host_t;

/* Read one DNS response. Returns how many hosts it held (up to cap), or 0 for
 * anything malformed. src_ipv4 (4 bytes, may be NULL) stands in for a missing
 * A record. */
int mdns_parse(const uint8_t *pkt, size_t len, const uint8_t *src_ipv4,
               mdns_host_t *out, int cap);

/* Non-blocking: call often. Opens the socket on first use and queries every
 * couple of seconds. Returns 1 when the host list changed. */
int mdns_poll(void);

/* Close the socket and forget the hosts. */
void mdns_stop(void);

/* Milliseconds since this PC last answered, matched by address or name; -1 if
 * it has not answered since discovery started. name may be NULL or empty. */
int mdns_host_age_ms(const char *addr, const char *name);

int mdns_count(void);
const mdns_host_t *mdns_host(int index);
