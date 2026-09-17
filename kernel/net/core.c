/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Network core: packet buffers, the interface list, the loopback interface,
 * Ethernet framing and the netrx thread that drives receive processing and
 * the protocol timers. */
#include "net/net.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"

spinlock_t net_lock = SPINLOCK_INIT;
const uint8_t eth_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- packets ------------------------------------------------------------------ */
struct pkt *pkt_alloc(void)
{
    struct pkt *p = kmalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->next = NULL;
    p->off = PKT_HEADROOM;
    p->len = 0;
    p->src_ip = p->dst_ip = 0;
    p->src_port = p->proto = p->ihl = 0;
    p->dev = NULL;
    return p;
}

void pkt_free(struct pkt *p)
{
    kfree(p);
}

/* ---- interfaces --------------------------------------------------------------- */
static struct netdev *devs;
static int next_index = 1;
static struct waitqueue netrx_wq = WAITQUEUE_INIT;
static volatile int netrx_pending;

int netdev_register(struct netdev *d)
{
    spin_lock(&net_lock);
    d->index = next_index++;
    d->next = NULL;
    if (d->mtu == 0)
        d->mtu = 1500;
    struct netdev **pp = &devs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = d;
    spin_unlock(&net_lock);
    kprintf("net: %s: %02x:%02x:%02x:%02x:%02x:%02x\n", d->name,
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    return 0;
}

struct netdev *netdev_first(void) { return devs; }

struct netdev *netdev_by_name(const char *name)
{
    for (struct netdev *d = devs; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

struct netdev *netdev_by_index(int index)
{
    for (struct netdev *d = devs; d; d = d->next)
        if (d->index == index)
            return d;
    return NULL;
}

void netdev_kick(struct netdev *d)
{
    d->rx_pending = 1;
    netrx_pending = 1;
    waitqueue_wake_all(&netrx_wq);
}

/* ---- Ethernet ----------------------------------------------------------------- */
struct eth_hdr {
    uint8_t  dst[6], src[6];
    uint16_t proto;
} __attribute__((packed));

int eth_send(struct netdev *d, const uint8_t *dst, uint16_t proto, struct pkt *p)
{
    if (d->flags & IFF_LOOPBACK) {
        p->proto = proto;           /* no framing on lo, just remember the protocol */
        return d->xmit(d, p);
    }
    struct eth_hdr *h = pkt_push(p, sizeof(*h));
    memcpy(h->dst, dst, 6);
    memcpy(h->src, d->mac, 6);
    h->proto = htons(proto);
    if (p->len < 60) {              /* minimum frame size */
        memset(pkt_data(p) + p->len, 0, 60 - p->len);
        p->len = 60;
    }
    d->tx_packets++;
    d->tx_bytes += p->len;
    return d->xmit(d, p);
}

/* A received frame. Called by drivers from their poll() with net_lock held;
 * the packet is consumed. */
void net_rx(struct netdev *d, struct pkt *p)
{
    p->dev = d;
    d->rx_packets++;
    d->rx_bytes += p->len;
    uint16_t proto;
    if (d->flags & IFF_LOOPBACK) {
        proto = p->proto;
    } else {
        if (p->len < sizeof(struct eth_hdr)) {
            d->rx_dropped++;
            pkt_free(p);
            return;
        }
        struct eth_hdr *h = (void *)pkt_data(p);
        proto = ntohs(h->proto);
        pkt_pull(p, sizeof(*h));
    }
    switch (proto) {
    case ETH_P_IP:  ip_rx(d, p); break;
    case ETH_P_ARP: arp_rx(d, p); break;
    default:        pkt_free(p); break;
    }
}

/* ---- loopback ----------------------------------------------------------------- */
static struct netdev lo;
static struct pkt *lo_head, *lo_tail;
static spinlock_t lo_lock = SPINLOCK_INIT;

static int lo_xmit(struct netdev *d, struct pkt *p)
{
#ifdef NET_LOSS_TEST
    /* make CFLAGS_EXTRA=-DNET_LOSS_TEST: drop every 5th TCP data segment on
     * lo to exercise retransmission, SACK and reassembly in the self test */
    static int counter;
    if (p->proto == ETH_P_IP && p->len > 40 && pkt_data(p)[9] == IPPROTO_TCP) {
        uint32_t ihl = (pkt_data(p)[0] & 0xF) * 4, doff = (pkt_data(p)[ihl + 12] >> 4) * 4;
        if (p->len > ihl + doff && ++counter % 5 == 0) {
            d->tx_errors++;
            pkt_free(p);
            return 0;
        }
    }
#endif
    uint64_t f = spin_lock_irqsave(&lo_lock);
    p->next = NULL;
    if (lo_tail) lo_tail->next = p; else lo_head = p;
    lo_tail = p;
    spin_unlock_irqrestore(&lo_lock, f);
    d->tx_packets++;
    d->tx_bytes += p->len;
    netdev_kick(d);
    return 0;
}

static void lo_poll(struct netdev *d)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&lo_lock);
        struct pkt *p = lo_head;
        if (p) {
            lo_head = p->next;
            if (!lo_head) lo_tail = NULL;
        }
        spin_unlock_irqrestore(&lo_lock, f);
        if (!p)
            return;
        net_rx(d, p);
    }
}

/* ---- the netrx thread --------------------------------------------------------- */
/* Receive processing and timers run here rather than in interrupt context:
 * drivers hand frames over with netdev_kick(), and the stack may allocate
 * memory and take net_lock freely. */
static void netrx_thread(void *arg)
{
    (void)arg;
    uint64_t last_timer = 0;
    for (;;) {
        wait_event_timeout(&netrx_wq, netrx_pending, 20);
        netrx_pending = 0;
        spin_lock(&net_lock);
        for (struct netdev *d = devs; d; d = d->next)
            if (d->rx_pending && d->poll) {
                d->rx_pending = 0;
                d->poll(d);
            }
        uint64_t now = timer_ms();
        if (now - last_timer >= 20) {
            last_timer = now;
            tcp_timer(now);
            arp_timer(now);
        }
        spin_unlock(&net_lock);
    }
}

void net_init(void)
{
    strcpy(lo.name, "lo");
    lo.mtu = 65535;
    lo.flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    lo.ip = IP4(127, 0, 0, 1);
    lo.mask = IP4(255, 0, 0, 0);
    lo.xmit = lo_xmit;
    lo.poll = lo_poll;
    netdev_register(&lo);
    task_create("netrx", netrx_thread, NULL);
}
