/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ARP: a small cache with one packet parked per unresolved entry. */
#include "net/net.h"
#include "arch/x86_64/timer.h"
#include "string.h"

struct arp_hdr {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ARP_ENTRIES 32
#define ARP_TTL_MS  (10 * 60 * 1000)
#define ARP_RETRY_MS 1000

struct arp_entry {
    struct netdev *dev;
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
    uint64_t updated, last_request;
    struct pkt *pending;
};
static struct arp_entry cache[ARP_ENTRIES];

static struct arp_entry *arp_find(struct netdev *d, uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (cache[i].dev == d && cache[i].ip == ip && (cache[i].valid || cache[i].pending))
            return &cache[i];
    return NULL;
}

static struct arp_entry *arp_slot(struct netdev *d, uint32_t ip)
{
    struct arp_entry *e = arp_find(d, ip);
    if (e)
        return e;
    struct arp_entry *oldest = &cache[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!cache[i].valid && !cache[i].pending) { oldest = &cache[i]; break; }
        if (cache[i].updated < oldest->updated) oldest = &cache[i];
    }
    if (oldest->pending)
        pkt_free(oldest->pending);
    memset(oldest, 0, sizeof(*oldest));
    oldest->dev = d;
    oldest->ip = ip;
    return oldest;
}

static void arp_send(struct netdev *d, uint16_t op, const uint8_t *dst_mac, const uint8_t *tha, uint32_t tpa)
{
    struct pkt *p = pkt_alloc();
    if (!p)
        return;
    struct arp_hdr *a = pkt_push(p, sizeof(*a));
    a->htype = htons(1);
    a->ptype = htons(ETH_P_IP);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, d->mac, 6);
    a->spa = d->ip;
    memcpy(a->tha, tha, 6);
    a->tpa = tpa;
    eth_send(d, dst_mac, ETH_P_ARP, p);
}

/* Send p to nexthop on d, resolving the MAC first if needed. Takes p. */
int arp_output(struct netdev *d, uint32_t nexthop, struct pkt *p)
{
    if (nexthop == 0xFFFFFFFF || (d->mask && nexthop == netdev_bcast(d)))
        return eth_send(d, eth_broadcast, ETH_P_IP, p);
    struct arp_entry *e = arp_slot(d, nexthop);
    if (e->valid)
        return eth_send(d, e->mac, ETH_P_IP, p);
    if (e->pending)
        pkt_free(e->pending);       /* keep the newest packet only */
    e->pending = p;
    uint64_t now = timer_ms();
    if (now - e->last_request >= ARP_RETRY_MS || e->last_request == 0) {
        e->last_request = now;
        static const uint8_t zero[6];
        arp_send(d, ARP_REQUEST, eth_broadcast, zero, nexthop);
    }
    return 0;
}

static void arp_learn(struct netdev *d, uint32_t ip, const uint8_t *mac)
{
    struct arp_entry *e = arp_slot(d, ip);
    memcpy(e->mac, mac, 6);
    e->valid = 1;
    e->updated = timer_ms();
    if (e->pending) {
        struct pkt *p = e->pending;
        e->pending = NULL;
        eth_send(d, e->mac, ETH_P_IP, p);
    }
}

void arp_rx(struct netdev *d, struct pkt *p)
{
    struct arp_hdr *a = (void *)pkt_data(p);
    if (p->len < sizeof(*a) || ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP || a->hlen != 6 || a->plen != 4) {
        pkt_free(p);
        return;
    }
    int for_us = d->ip && a->tpa == d->ip;
    /* Learn from anything addressed to us, and from replies we asked for. */
    if (for_us || arp_find(d, a->spa))
        arp_learn(d, a->spa, a->sha);
    if (for_us && ntohs(a->op) == ARP_REQUEST)
        arp_send(d, ARP_REPLY, a->sha, a->sha, a->spa);
    pkt_free(p);
}

void arp_timer(uint64_t now)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &cache[i];
        if (e->valid && now - e->updated > ARP_TTL_MS)
            e->valid = 0;
        if (e->pending && now - e->last_request > 5 * ARP_RETRY_MS) {
            pkt_free(e->pending);   /* nobody answered: give up on the parked packet */
            e->pending = NULL;
        } else if (e->pending && now - e->last_request >= ARP_RETRY_MS) {
            e->last_request = now;
            static const uint8_t zero[6];
            arp_send(e->dev, ARP_REQUEST, eth_broadcast, zero, e->ip);
        }
    }
}
