/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* IPv4 (no fragmentation, no options on output) and ICMP echo. */
#include "net/net.h"
#include "string.h"
#include "printf.h"

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint8_t *b = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)(b[0] << 8 | b[1]);
        b += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(b[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint16_t ip_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len)
{
    const uint8_t *b = data;
    uint32_t sum = 0;
    sum += (ntohl(src) >> 16) + (ntohl(src) & 0xFFFF);
    sum += (ntohl(dst) >> 16) + (ntohl(dst) & 0xFFFF);
    sum += proto;
    sum += (uint32_t)len;
    size_t n = len;
    while (n > 1) {
        sum += (uint32_t)(b[0] << 8 | b[1]);
        b += 2;
        n -= 2;
    }
    if (n)
        sum += (uint32_t)(b[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

int ip_local(uint32_t addr)
{
    if ((ntohl(addr) >> 24) == 127)
        return 1;
    for (struct netdev *d = netdev_first(); d; d = d->next)
        if (d->ip && d->ip == addr)
            return 1;
    return 0;
}

/* Pick the interface and next hop for dst. */
struct netdev *ip_route(uint32_t dst, uint32_t *nexthop)
{
    struct netdev *lo = netdev_first();          /* lo registers first */
    if (ip_local(dst)) {
        *nexthop = dst;
        return lo;
    }
    struct netdev *gw = NULL;
    for (struct netdev *d = lo->next; d; d = d->next) {
        if (!(d->flags & IFF_UP))
            continue;
        if (dst == 0xFFFFFFFF) {                 /* limited broadcast: first interface up */
            *nexthop = dst;
            return d;
        }
        if (d->ip && d->mask && (dst & d->mask) == (d->ip & d->mask)) {
            *nexthop = dst;
            return d;
        }
        if (d->gw && !gw)
            gw = d;
    }
    if (gw) {
        *nexthop = gw->gw;
        return gw;
    }
    return NULL;
}

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, struct pkt *p)
{
    static uint16_t ident;
    uint32_t nexthop;
    struct netdev *d = ip_route(dst, &nexthop);
    if (!d) {
        pkt_free(p);
        return -ENETUNREACH;
    }
    if (!src)
        src = d->ip;
    if (p->len + sizeof(struct ip_hdr) > d->mtu) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    struct ip_hdr *h = pkt_push(p, sizeof(*h));
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->len = htons((uint16_t)p->len);
    h->id = htons(ident++);
    h->frag = htons(0x4000);                     /* don't fragment */
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = src;
    h->dst = dst;
    h->csum = ip_checksum(h, sizeof(*h));
    return arp_output(d, nexthop, p);
}

void ip_rx(struct netdev *d, struct pkt *p)
{
    struct ip_hdr *h = (void *)pkt_data(p);
    if (p->len < sizeof(*h) || (h->ver_ihl >> 4) != 4)
        goto drop;
    uint32_t ihl = (h->ver_ihl & 0xF) * 4;
    uint32_t total = ntohs(h->len);
    if (ihl < 20 || total < ihl || total > p->len)
        goto drop;
    if (ip_checksum(h, ihl) != 0)
        goto drop;
    if (ntohs(h->frag) & 0x3FFF)                 /* fragments: not supported */
        goto drop;
    /* For us: our address, a broadcast, or anything while unconfigured (DHCP). */
    int bcast = h->dst == 0xFFFFFFFF || (d->ip && d->mask && h->dst == netdev_bcast(d));
    if (!bcast && !ip_local(h->dst) && d->ip != 0)
        goto drop;
    p->len = total;                              /* strip Ethernet padding */
    p->src_ip = h->src;
    p->dst_ip = h->dst;
    p->proto = h->proto;
    p->ihl = (uint16_t)ihl;
    pkt_pull(p, ihl);
    switch (h->proto) {
    case IPPROTO_ICMP: icmp_rx(p); return;
    case IPPROTO_UDP:  udp_rx(p); return;
    case IPPROTO_TCP:  tcp_rx(p); return;
    }
drop:
    d->rx_dropped++;
    pkt_free(p);
}

/* ---- ICMP --------------------------------------------------------------------- */
struct icmp_hdr {
    uint8_t  type, code;
    uint16_t csum;
    uint32_t rest;
} __attribute__((packed));

void icmp_rx(struct pkt *p)
{
    struct icmp_hdr *h = (void *)pkt_data(p);
    if (p->len < sizeof(*h) || ip_checksum(h, p->len) != 0) {
        pkt_free(p);
        return;
    }
    /* Raw ICMP sockets get a copy of everything, IP header included (like Linux). */
    for (struct socket *s = raw_sockets; s; s = s->next) {
        if (s->proto != IPPROTO_ICMP || (s->remote_ip && s->remote_ip != p->src_ip))
            continue;
        struct pkt *c = pkt_alloc();
        if (!c)
            continue;
        uint32_t ihl = p->ihl;
        c->off = PKT_HEADROOM;
        c->len = p->len + ihl;
        memcpy(pkt_data(c), pkt_data(p) - ihl, c->len);   /* the header sits right in front */
        c->src_ip = p->src_ip;
        sock_deliver(s, c);
    }
    if (h->type == 8 && h->code == 0) {          /* echo request -> reply in place */
        uint32_t src = p->src_ip, dst = p->dst_ip;
        h->type = 0;
        h->csum = 0;
        h->csum = ip_checksum(h, p->len);
        ip_send(ip_local(dst) ? dst : 0, src, IPPROTO_ICMP, p);
        return;
    }
    pkt_free(p);
}
