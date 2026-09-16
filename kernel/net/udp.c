/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* UDP: demultiplexing to bound sockets; sending is done in socket.c. */
#include "net/net.h"
#include "string.h"

struct udp_hdr {
    uint16_t src, dst, len, csum;
} __attribute__((packed));

uint16_t udp_ephemeral_port(void)
{
    static uint16_t next = 49152;
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t port = next++;
        if (next == 0) next = 49152;
        int used = 0;
        for (struct socket *s = udp_sockets; s; s = s->next)
            if (s->local_port == htons(port)) { used = 1; break; }
        if (!used)
            return htons(port);
    }
    return 0;
}

void udp_rx(struct pkt *p)
{
    struct udp_hdr *h = (void *)pkt_data(p);
    if (p->len < sizeof(*h) || ntohs(h->len) < sizeof(*h) || ntohs(h->len) > p->len) {
        pkt_free(p);
        return;
    }
    if (h->csum && ip_pseudo_checksum(p->src_ip, p->dst_ip, IPPROTO_UDP, h, ntohs(h->len)) != 0) {
        pkt_free(p);
        return;
    }
    p->len = ntohs(h->len);
    p->src_port = h->src;
    uint16_t dport = h->dst;
    pkt_pull(p, sizeof(*h));

    /* Best match: a socket bound to this port whose local address is
     * unspecified or equal to the destination, connected sockets only
     * from their peer. */
    for (struct socket *s = udp_sockets; s; s = s->next) {
        if (s->local_port != dport)
            continue;
        if (s->local_ip && s->local_ip != p->dst_ip && !(s->local_ip == p->dev->ip && p->dst_ip == 0xFFFFFFFF))
            continue;
        if (s->connected && (s->remote_ip != p->src_ip || s->remote_port != p->src_port))
            continue;
        sock_deliver(s, p);
        return;
    }
    pkt_free(p);                    /* no listener: silently dropped (no ICMP unreachable) */
}

/* Build and send one datagram. Payload already in p. */
int udp_send(struct socket *s, uint32_t dst_ip, uint16_t dst_port, struct pkt *p)
{
    struct udp_hdr *h = pkt_push(p, sizeof(*h));
    uint32_t nexthop;
    struct netdev *d = ip_route(dst_ip, &nexthop);
    if (!d) {
        pkt_free(p);
        return -ENETUNREACH;
    }
    uint32_t src = s->local_ip ? s->local_ip : d->ip;
    h->src = s->local_port;
    h->dst = dst_port;
    h->len = htons((uint16_t)p->len);
    h->csum = 0;
    h->csum = ip_pseudo_checksum(src, dst_ip, IPPROTO_UDP, h, p->len);
    if (h->csum == 0)
        h->csum = 0xFFFF;
    return ip_send(src, dst_ip, IPPROTO_UDP, p);
}
