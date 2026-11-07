#include "mdns.h"
#include "../log.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

#define SERVICE_NAME "_nvstream._tcp.local"
#define MDNS_PORT 5353
#define DNS_HEADER_LEN 12
#define DNS_TYPE_A 1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_SRV 33
#define MAX_NAME 128
#define MAX_RECORDS 32
#define MAX_JUMPS 16
#define QUERY_INTERVAL_US 2000000ull

/* Absent from OpenOrbis' Net.h. The kernel socket ABI is FreeBSD's. */
#define ML_NET_IPPROTO_UDP 17
#define ML_NET_IPPROTO_IP 0
#define ML_NET_SOL_SOCKET 0xffff
#define ML_NET_SO_REUSEADDR 0x0004
#define ML_NET_SO_REUSEPORT 0x0200
#define ML_NET_SO_NBIO 0x1200
#define ML_NET_IP_ADD_MEMBERSHIP 12
#define ML_NET_IP_ADD_MEMBERSHIP_ALT 35

typedef struct {
    uint8_t len;
    uint8_t family;
    uint16_t port;
    uint8_t addr[4];
    uint8_t zero[8];
} ml_sockaddr_in_t;

typedef struct {
    uint8_t multiaddr[4];
    uint8_t interface_addr[4];
} ml_ip_mreq_t;

static const uint8_t k_group[4] = { 224, 0, 0, 251 };

static OrbisNetId s_sock = -1;
static int s_open_failed;
static uint64_t s_last_query_us;
static mdns_host_t s_hosts[MDNS_MAX_HOSTS];
static int s_count;

/* --- parsing --- */

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Read a possibly compressed name as dotted text. Returns the offset just past
 * the name in the record, or -1 if malformed. */
static int read_name(const uint8_t *pkt, size_t len, size_t off, char *out, size_t cap) {
    size_t w = 0;
    int next = -1;
    int jumps = 0;
    out[0] = '\0';
    for (;;) {
        if (off >= len)
            return -1;
        uint8_t c = pkt[off];
        if (c == 0) {
            if (next < 0)
                next = (int)off + 1;
            break;
        }
        if ((c & 0xC0) == 0xC0) {
            if (off + 1 >= len || ++jumps > MAX_JUMPS)
                return -1;
            if (next < 0)
                next = (int)off + 2;
            off = ((size_t)(c & 0x3F) << 8) | pkt[off + 1];
            continue;
        }
        if (c & 0xC0)
            return -1;
        if (off + 1 + c > len)
            return -1;
        if (w + c + 2 > cap)
            return -1;
        if (w)
            out[w++] = '.';
        memcpy(out + w, pkt + off + 1, c);
        w += c;
        out[w] = '\0';
        off += 1u + c;
    }
    return next;
}

typedef struct {
    char name[MAX_NAME];
    uint16_t type;
    uint16_t rdlen;
    size_t rdoff;
} record_t;

int mdns_parse(const uint8_t *pkt, size_t len, const uint8_t *src_ipv4,
               mdns_host_t *out, int cap) {
    if (len < DNS_HEADER_LEN || !(pkt[2] & 0x80)) /* QR bit: responses only */
        return 0;

    int questions = read_u16(pkt + 4);
    int records = read_u16(pkt + 6) + read_u16(pkt + 8) + read_u16(pkt + 10);
    size_t off = DNS_HEADER_LEN;
    char name[MAX_NAME];

    for (int i = 0; i < questions; i++) {
        int next = read_name(pkt, len, off, name, sizeof(name));
        if (next < 0 || (size_t)next + 4 > len)
            return 0;
        off = (size_t)next + 4;
    }

    static record_t recs[MAX_RECORDS]; /* too big for the stack of a menu frame */
    int nrecs = 0;
    for (int i = 0; i < records && nrecs < MAX_RECORDS; i++) {
        int next = read_name(pkt, len, off, recs[nrecs].name, sizeof(recs[nrecs].name));
        if (next < 0 || (size_t)next + 10 > len)
            return 0;
        off = (size_t)next;
        recs[nrecs].type = read_u16(pkt + off);
        recs[nrecs].rdlen = read_u16(pkt + off + 8);
        recs[nrecs].rdoff = off + 10;
        if (recs[nrecs].rdoff + recs[nrecs].rdlen > len)
            return 0;
        off = recs[nrecs].rdoff + recs[nrecs].rdlen;
        nrecs++;
    }

    int found = 0;
    for (int i = 0; i < nrecs && found < cap; i++) {
        if (recs[i].type != DNS_TYPE_PTR || strcasecmp(recs[i].name, SERVICE_NAME) != 0)
            continue;
        char instance[MAX_NAME];
        if (read_name(pkt, len, recs[i].rdoff, instance, sizeof(instance)) < 0)
            continue;

        /* instance -> SRV -> target host -> A */
        char target[MAX_NAME];
        target[0] = '\0';
        for (int j = 0; j < nrecs; j++) {
            if (recs[j].type == DNS_TYPE_SRV && recs[j].rdlen > 6 &&
                !strcasecmp(recs[j].name, instance) &&
                read_name(pkt, len, recs[j].rdoff + 6, target, sizeof(target)) >= 0)
                break;
        }
        const uint8_t *ip = src_ipv4;
        for (int j = 0; target[0] && j < nrecs; j++) {
            if (recs[j].type == DNS_TYPE_A && recs[j].rdlen == 4 &&
                !strcasecmp(recs[j].name, target)) {
                ip = pkt + recs[j].rdoff;
                break;
            }
        }
        if (!ip)
            continue;

        mdns_host_t *h = &out[found++];
        /* The instance's first label is the PC name. */
        const char *dot = strchr(instance, '.');
        size_t n = dot ? (size_t)(dot - instance) : strlen(instance);
        if (n >= sizeof(h->name))
            n = sizeof(h->name) - 1;
        memcpy(h->name, instance, n);
        h->name[n] = '\0';
        snprintf(h->addr, sizeof(h->addr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    return found;
}

/* --- socket --- */

static int set_int_opt(int level, int opt, int value, const char *what) {
    int rc = sceNetSetsockopt(s_sock, level, opt, &value, sizeof(value));
    if (rc < 0)
        LOGW("mdns: setsockopt %s => 0x%08x", what, (unsigned)rc);
    return rc;
}

static void open_socket(void) {
    s_sock = sceNetSocket("mdns", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, ML_NET_IPPROTO_UDP);
    if (s_sock < 0) {
        LOGW("mdns: socket => 0x%08x; discovery off", (unsigned)s_sock);
        s_open_failed = 1;
        return;
    }
    (void)set_int_opt(ML_NET_SOL_SOCKET, ML_NET_SO_REUSEADDR, 1, "REUSEADDR");
    (void)set_int_opt(ML_NET_SOL_SOCKET, ML_NET_SO_REUSEPORT, 1, "REUSEPORT");
    if (set_int_opt(ML_NET_SOL_SOCKET, ML_NET_SO_NBIO, 1, "NBIO") < 0) {
        /* Blocking reads would freeze the menu: better no discovery. */
        sceNetSocketClose(s_sock);
        s_sock = -1;
        s_open_failed = 1;
        return;
    }

    ml_sockaddr_in_t local;
    memset(&local, 0, sizeof(local));
    local.len = sizeof(local);
    local.family = ORBIS_NET_AF_INET;
    local.port = sceNetHtons(MDNS_PORT);
    int rc = sceNetBind(s_sock, (const OrbisNetSockaddr *)&local, sizeof(local));
    if (rc < 0)
        LOGW("mdns: bind 5353 => 0x%08x", (unsigned)rc);

    /* Hosts reply to the group: Windows did even for a query from another port. */
    ml_ip_mreq_t mreq;
    memcpy(mreq.multiaddr, k_group, 4);
    memset(mreq.interface_addr, 0, 4);
    rc = sceNetSetsockopt(s_sock, ML_NET_IPPROTO_IP, ML_NET_IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (rc < 0) /* OpenOrbis' own netinet/in.h says 35 (the Linux value). */
        rc = sceNetSetsockopt(s_sock, ML_NET_IPPROTO_IP, ML_NET_IP_ADD_MEMBERSHIP_ALT, &mreq, sizeof(mreq));
    LOGI("mdns: socket %d, join group => 0x%08x", s_sock, (unsigned)rc);
}

static void send_query(void) {
    /* Header: one question. Then _nvstream._tcp.local, type PTR, class IN. */
    static const uint8_t k_query[] = {
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        9, '_', 'n', 'v', 's', 't', 'r', 'e', 'a', 'm',
        4, '_', 't', 'c', 'p',
        5, 'l', 'o', 'c', 'a', 'l',
        0,
        0, DNS_TYPE_PTR, 0, 1,
    };
    ml_sockaddr_in_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.len = sizeof(dst);
    dst.family = ORBIS_NET_AF_INET;
    dst.port = sceNetHtons(MDNS_PORT);
    memcpy(dst.addr, k_group, 4);
    int rc = sceNetSendto(s_sock, k_query, sizeof(k_query), 0,
                          (const OrbisNetSockaddr *)&dst, sizeof(dst));
    if (rc < 0)
        LOGW("mdns: send => 0x%08x", (unsigned)rc);
}

static int add_host(const mdns_host_t *h, uint64_t now) {
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_hosts[i].addr, h->addr)) {
            s_hosts[i].last_seen_us = now;
            if (strcmp(s_hosts[i].name, h->name)) {
                snprintf(s_hosts[i].name, sizeof(s_hosts[i].name), "%s", h->name);
                return 1;
            }
            return 0;
        }
    }
    if (s_count >= MDNS_MAX_HOSTS)
        return 0;
    s_hosts[s_count] = *h;
    s_hosts[s_count++].last_seen_us = now;
    LOGI("mdns: found '%s' at %s", h->name, h->addr);
    return 1;
}

int mdns_poll(void) {
    if (s_open_failed)
        return 0;
    if (s_sock < 0) {
        open_socket();
        if (s_sock < 0)
            return 0;
        s_last_query_us = 0;
    }

    uint64_t now = sceKernelGetProcessTime();
    if (s_last_query_us == 0 || now - s_last_query_us > QUERY_INTERVAL_US) {
        s_last_query_us = now;
        send_query();
    }

    int changed = 0;
    static uint8_t buf[2048];
    /* A bound on packets per call: busy networks must not stall the menu. */
    for (int i = 0; i < 16; i++) {
        ml_sockaddr_in_t from;
        OrbisNetSocklen_t from_len = sizeof(from);
        memset(&from, 0, sizeof(from));
        int n = sceNetRecvfrom(s_sock, buf, sizeof(buf), 0, (OrbisNetSockaddr *)&from, &from_len);
        if (n <= 0)
            break;
        mdns_host_t found[MDNS_MAX_HOSTS];
        int count = mdns_parse(buf, (size_t)n, from.addr, found, MDNS_MAX_HOSTS);
        for (int h = 0; h < count; h++)
            changed |= add_host(&found[h], sceKernelGetProcessTime());
    }
    return changed;
}

void mdns_stop(void) {
    if (s_sock >= 0)
        sceNetSocketClose(s_sock);
    s_sock = -1;
    s_open_failed = 0;
    s_count = 0;
}

int mdns_host_age_ms(const char *addr, const char *name) {
    for (int i = 0; i < s_count; i++) {
        const mdns_host_t *h = &s_hosts[i];
        if ((addr && !strcasecmp(h->addr, addr)) || (addr && !strcasecmp(h->name, addr)) ||
            (name && name[0] && !strcasecmp(h->name, name)))
            return (int)((sceKernelGetProcessTime() - h->last_seen_us) / 1000u);
    }
    return -1;
}

int mdns_count(void) {
    return s_count;
}

const mdns_host_t *mdns_host(int index) {
    return &s_hosts[index];
}
