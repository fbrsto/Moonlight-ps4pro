#include "wol.h"
#include "../log.h"

#include <stdio.h>
#include <string.h>

#include <orbis/Net.h>

/* Absent from OpenOrbis' Net.h. The kernel socket ABI is FreeBSD's. */
#define ML_NET_IPPROTO_UDP 17
#define ML_NET_SOL_SOCKET 0xffff
#define ML_NET_SO_BROADCAST 0x0020

typedef struct {
    uint8_t len;
    uint8_t family;
    uint16_t port;
    uint8_t addr[4];
    uint8_t zero[8];
} wol_sockaddr_in_t;

int wol_build_packet(const char *mac, uint8_t out[WOL_PACKET_SIZE]) {
    unsigned b[6];
    char end;
    if (!mac || sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6)
        return -1;
    if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5]))
        return -1;
    memset(out, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 6; j++)
            out[6 + i * 6 + j] = (uint8_t)b[j];
    return 0;
}

int wol_send(const char *mac, const char *host) {
    /* The usual WoL port, then the ports the official client also uses so the
     * packet can get through a router's port forwards. */
    static const uint16_t k_ports[] = { 9, 47009, 47998, 47999, 48000, 48002, 48010 };
    uint8_t packet[WOL_PACKET_SIZE];
    if (wol_build_packet(mac, packet) != 0) {
        LOGW("wol: no usable MAC ('%s')", mac ? mac : "");
        return 0;
    }

    OrbisNetId s = sceNetSocket("wol", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, ML_NET_IPPROTO_UDP);
    if (s < 0) {
        LOGW("wol: socket => 0x%08x", (unsigned)s);
        return 0;
    }
    int on = 1;
    int rc = sceNetSetsockopt(s, ML_NET_SOL_SOCKET, ML_NET_SO_BROADCAST, &on, sizeof(on));
    if (rc < 0)
        LOGW("wol: SO_BROADCAST => 0x%08x", (unsigned)rc);

    uint8_t targets[2][4] = { { 255, 255, 255, 255 } };
    int ntargets = 1;
    unsigned a, b, c, d;
    char end;
    if (host && sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) == 4 && a < 256 && b < 256 && c < 256) {
        targets[1][0] = (uint8_t)a;
        targets[1][1] = (uint8_t)b;
        targets[1][2] = (uint8_t)c;
        targets[1][3] = 255;
        ntargets = 2;
    }

    int sent = 0;
    for (int t = 0; t < ntargets; t++) {
        for (size_t p = 0; p < sizeof(k_ports) / sizeof(k_ports[0]); p++) {
            wol_sockaddr_in_t dst;
            memset(&dst, 0, sizeof(dst));
            dst.len = sizeof(dst);
            dst.family = ORBIS_NET_AF_INET;
            dst.port = sceNetHtons(k_ports[p]);
            memcpy(dst.addr, targets[t], 4);
            if (sceNetSendto(s, packet, sizeof(packet), 0, (const OrbisNetSockaddr *)&dst, sizeof(dst)) >= 0)
                sent++;
        }
    }
    sceNetSocketClose(s);
    LOGI("wol: magic packet for %s: %d sends", mac, sent);
    return sent;
}
