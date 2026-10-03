/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Talking to a Qualcomm phone's modem once it runs, the layers Linux's
 * smp2p, qcom_glink_smem/_native and qrtr (with its name service) are:
 *   SMP2P   state bits both ways in SMEM items (ours 428, the modem's 435)
 *   GLINK   two 16 KiB FIFOs in SMEM (479 ours to it, 480 its to us) and a
 *           descriptor (478) with their heads and tails; channels on top
 *   QRTR    packets on the GLINK channel "IPCRTR": nodes, ports, servers
 * The modem's software asks for its services (the remote file system,
 * later the WLAN's) through QRTR. Everything here is polled by one thread,
 * started when the modem is: no interrupts yet. The apps processor kicks
 * the modem through the APCS IPC register. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/qcom_smem.h"
#include "asm/qcom_ipc.h"
#include "asm/timer.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

#define MODEM_HOST          1
#define APCS_GLB            0x0f111000
#define APCS_IPC            8               /* the IPC register (msm8994_apcs_data) */
#define IPC_GLINK_MODEM     12
#define IPC_SMP2P_MODEM     14

static void kick(int bit) { mmio_write32((volatile uint8_t *)P2V(APCS_GLB + APCS_IPC), 1u << bit); }

/* ---- SMP2P ------------------------------------------------------------------------------- */

#define SMP2P_OUT_ITEM      428
#define SMP2P_IN_ITEM       435
#define SMP2P_MAGIC         0x504d5324      /* "$SMP" */
#define SMP2P_ENTRIES       16

static volatile uint8_t *smp2p_out;

/* Our item, as Linux's qcom_smp2p_alloc_outbound_item, with one entry
 * ("master-kernel": the bit the modem is told to stop with). Before the
 * modem starts. */
int qcom_smp2p_init(void)
{
    size_t sz = 20 + 20 * SMP2P_ENTRIES;
    if (smem_alloc(MODEM_HOST, SMP2P_OUT_ITEM, sz)) return -1;
    smp2p_out = smem_get(MODEM_HOST, SMP2P_OUT_ITEM, &sz);
    if (!smp2p_out || sz < 20 + 20 * SMP2P_ENTRIES) return -1;
    volatile uint32_t *w = (volatile uint32_t *)smp2p_out;
    w[1] = 0;                                                   /* version 0 while it is set up */
    __asm__ volatile("dsb sy" ::: "memory");
    w[0] = SMP2P_MAGIC;
    w[2] = 0 | (uint32_t)MODEM_HOST << 16;                      /* local pid 0 (apps), remote pid */
    w[3] = SMP2P_ENTRIES;                                       /* total, 0 valid */
    w[4] = 0;                                                   /* flags */
    static const char name[16] = "master-kernel";
    for (int k = 0; k < 16; k += 4) { uint32_t c; memcpy(&c, name + k, 4); w[5 + k / 4] = c; }
    w[9] = 0;                                                   /* its value */
    w[3] = SMP2P_ENTRIES | 1u << 16;
    __asm__ volatile("dsb sy" ::: "memory");
    w[1] = 1 | 1u << 8;                                         /* version 1, features: SSR ack */
    __asm__ volatile("dsb sy" ::: "memory");
    kick(IPC_SMP2P_MODEM);
    return 0;
}

/* The modem's entry `name` (its "slave-kernel": fatal, ready, handover,
 * stop-ack bits), or -1 while it has none. */
int64_t qcom_smp2p_in(const char *name)
{
    size_t sz;
    const volatile uint8_t *it = smem_get(MODEM_HOST, SMP2P_IN_ITEM, &sz);
    if (!it || sz < 20 || mmio_read32(it) != SMP2P_MAGIC) return -1;
    unsigned valid = mmio_read32(it + 12) >> 16;
    for (unsigned i = 0; i < valid && 20 + 20 * (i + 1) <= sz; i++) {
        char n[17];
        for (int k = 0; k < 16; k += 4) { uint32_t c = mmio_read32(it + 20 + 20 * i + k); memcpy(n + k, &c, 4); }
        n[16] = 0;
        if (strcmp(n, name) == 0) return mmio_read32(it + 20 + 20 * i + 16);
    }
    return -1;
}

/* ---- GLINK over SMEM ------------------------------------------------------------------- */

#define GLINK_DESC          478
#define GLINK_FIFO_TX       479
#define GLINK_FIFO_RX       480
#define GLINK_FIFO_SIZE     16384

enum {
    CMD_VERSION = 0, CMD_VERSION_ACK = 1, CMD_OPEN = 2, CMD_CLOSE = 3, CMD_OPEN_ACK = 4, CMD_INTENT = 5,
    CMD_RX_DONE = 6, CMD_RX_INTENT_REQ = 7, CMD_RX_INTENT_REQ_ACK = 8, CMD_TX_DATA = 9, CMD_CLOSE_ACK = 11,
    CMD_TX_DATA_CONT = 12, CMD_READ_NOTIF = 13, CMD_RX_DONE_W_REUSE = 14, CMD_SIGNALS = 15,
};
#define FEATURE_INTENT_REUSE 1u

static volatile uint32_t *desc;             /* [0] tx tail, [1] tx head, [2] rx tail, [3] rx head */
static volatile uint8_t *tx_fifo, *rx_fifo;
static uint32_t tx_len, rx_len;
static uint32_t features = FEATURE_INTENT_REUSE;
static int link_version;                    /* 1 once the modem acknowledged ours */

#define MAX_INTENTS 16
struct intent { uint32_t id, size, used; uint8_t *buf; };
struct pending { struct pending *next; size_t len; uint8_t data[]; };

struct chan {
    char name[32];
    uint16_t lcid, rcid;
    int remote_open, local_open, acked, up;
    struct intent ours[MAX_INTENTS];        /* buffers we offered it */
    int nours;
    struct intent theirs[MAX_INTENTS];      /* buffers it offered us */
    int ntheirs;
    uint32_t rx_iid, rx_off;                /* a message arriving in pieces */
    struct pending *queue;                  /* waiting for an intent */
    int intent_asked;
    void (*on_up)(struct chan *c);
    void (*on_rx)(struct chan *c, const uint8_t *data, size_t len);
    uint32_t rx_count, tx_count;
};

#define MAX_CHANS 4
static struct chan chans[MAX_CHANS];
static int nchans;
static uint16_t next_lcid = 1;

static uint32_t rx_avail(void)
{
    uint32_t head = desc[3], tail = desc[2];
    return head >= tail ? head - tail : rx_len - tail + head;
}

static void rx_peek(void *dst, uint32_t off, uint32_t n)
{
    uint32_t t = desc[2] + off;
    while (t >= rx_len) t -= rx_len;
    __asm__ volatile("dmb ld" ::: "memory");
    for (uint32_t i = 0; i < n; i++) {
        ((uint8_t *)dst)[i] = rx_fifo[t];
        if (++t == rx_len) t = 0;
    }
}

static void rx_advance(uint32_t n)
{
    uint32_t t = desc[2] + n;
    while (t >= rx_len) t -= rx_len;
    __asm__ volatile("dmb sy" ::: "memory");
    desc[2] = t;
}

static uint32_t tx_avail(void)
{
    uint32_t head = desc[1], tail = desc[0];
    uint32_t avail = tail <= head ? tx_len - head + tail : tail - head;
    return avail < 16 ? 0 : avail - 16;
}

static uint32_t tx_put(uint32_t head, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        tx_fifo[head] = ((const uint8_t *)src)[i];
        if (++head == tx_len) head = 0;
    }
    return head;
}

/* A message (header + payload) into the FIFO, if it fits now. */
static int tx(const void *hdr, size_t hlen, const void *data, size_t dlen)
{
    if (!desc || tx_avail() < hlen + dlen + 8) return -1;
    uint32_t head = desc[1];
    head = tx_put(head, hdr, hlen);
    if (dlen) head = tx_put(head, data, dlen);
    head = (head + 7) & ~7u;
    if (head >= tx_len) head -= tx_len;
    __asm__ volatile("dsb sy" ::: "memory");
    desc[1] = head;
    __asm__ volatile("dsb sy" ::: "memory");
    kick(IPC_GLINK_MODEM);
    return 0;
}

static int tx_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
    uint32_t m[2] = { cmd | (uint32_t)p1 << 16, p2 };
    for (int i = 0; i < 200 && tx(m, 8, NULL, 0); i++) task_sleep_ms(1);
    return 0;
}

static void set_name(struct chan *c, const char *name)
{
    size_t n = strlen(name);
    if (n >= sizeof c->name) n = sizeof c->name - 1;
    memcpy(c->name, name, n);
    c->name[n] = 0;
}

static struct chan *chan_by_name(const char *name)
{
    for (int i = 0; i < nchans; i++) if (strcmp(chans[i].name, name) == 0) return &chans[i];
    return NULL;
}
static struct chan *chan_by_rcid(uint16_t rcid)
{
    for (int i = 0; i < nchans; i++) if (chans[i].remote_open && chans[i].rcid == rcid) return &chans[i];
    return NULL;
}
static struct chan *chan_by_lcid(uint16_t lcid)
{
    for (int i = 0; i < nchans; i++) if (chans[i].local_open && chans[i].lcid == lcid) return &chans[i];
    return NULL;
}

static void advertise(struct chan *c, struct intent *in)
{
    uint32_t m[4] = { CMD_INTENT | (uint32_t)c->lcid << 16, 1, in->size, in->id };
    for (int i = 0; i < 200 && tx(m, 16, NULL, 0); i++) task_sleep_ms(1);
}

static struct intent *add_intent(struct chan *c, uint32_t size)
{
    if (c->nours >= MAX_INTENTS) return NULL;
    struct intent *in = &c->ours[c->nours];
    in->buf = kmalloc(size);
    if (!in->buf) return NULL;
    in->id = (uint32_t)c->nours + 1;
    in->size = size;
    in->used = 0;
    c->nours++;
    return in;
}

/* Both sides have opened it: offer our buffers (the DT's qcom,intents
 * for IPCRTR: 5 of 2 KiB, 3 of 8 KiB, 2 of 17 KiB), then the user's up. */
static void chan_up(struct chan *c)
{
    static const uint32_t sizes[][2] = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } };
    c->up = 1;
    for (size_t g = 0; g < 3; g++)
        for (uint32_t k = 0; k < sizes[g][1]; k++) {
            struct intent *in = add_intent(c, sizes[g][0]);
            if (in) advertise(c, in);
        }
    kprintf("glink: channel %s open (ours %u, its %u)\n", c->name, c->lcid, c->rcid);
    if (c->on_up) c->on_up(c);
}

static void open_local(struct chan *c)
{
    if (c->local_open) return;
    c->local_open = 1;
    c->lcid = next_lcid++;
    size_t n = strlen(c->name) + 1;
    uint8_t m[8 + 32] = { 0 };
    uint32_t h[2] = { CMD_OPEN | (uint32_t)c->lcid << 16, (uint32_t)n };
    memcpy(m, h, 8);
    memcpy(m + 8, c->name, n);
    for (int i = 0; i < 200 && tx(m, 8 + ((n + 7) & ~(size_t)7), NULL, 0); i++) task_sleep_ms(1);
}

/* Data for a channel: into one of its intents (the smallest that fits);
 * queued until the modem offers one, asked for if none would do. */
static int chan_try_send(struct chan *c, const uint8_t *data, size_t len)
{
    struct intent *best = NULL;
    for (int i = 0; i < c->ntheirs; i++)
        if (!c->theirs[i].used && c->theirs[i].size >= len && (!best || c->theirs[i].size < best->size)) best = &c->theirs[i];
    if (!best) {
        if (!c->intent_asked) { tx_cmd(CMD_RX_INTENT_REQ, c->lcid, (uint32_t)len); c->intent_asked = 1; }
        return -1;
    }
    size_t chunks = (len + 8191) / 8192;
    if (tx_avail() < len + 24 * chunks + 8) return -1;
    best->used = 1;
    for (size_t off = 0; off < len; ) {
        size_t n = len - off > 8192 ? 8192 : len - off;
        uint32_t h[4] = { (off ? CMD_TX_DATA_CONT : CMD_TX_DATA) | (uint32_t)c->lcid << 16, best->id, (uint32_t)n, (uint32_t)(len - off - n) };
        while (tx(h, 16, data + off, n)) task_sleep_ms(1);
        off += n;
    }
    c->tx_count++;
    return 0;
}

int glink_send(struct chan *c, const void *data, size_t len)
{
    if (!c->up) return -1;
    if (!c->queue && chan_try_send(c, data, len) == 0) return 0;
    struct pending *p = kmalloc(sizeof *p + len);
    if (!p) return -1;
    p->next = NULL; p->len = len;
    memcpy(p->data, data, len);
    struct pending **pp = &c->queue;
    while (*pp) pp = &(*pp)->next;
    *pp = p;
    return 0;
}

static void flush_queues(void)
{
    for (int i = 0; i < nchans; i++) {
        struct chan *c = &chans[i];
        while (c->up && c->queue && chan_try_send(c, c->queue->data, c->queue->len) == 0) {
            struct pending *p = c->queue;
            c->queue = p->next;
            kfree(p);
        }
    }
}

struct chan *glink_channel(const char *name, void (*on_up)(struct chan *), void (*on_rx)(struct chan *, const uint8_t *, size_t))
{
    struct chan *c = chan_by_name(name);
    if (!c && nchans < MAX_CHANS) { c = &chans[nchans++]; memset(c, 0, sizeof *c); set_name(c, name); }
    if (c) { c->on_up = on_up; c->on_rx = on_rx; }
    return c;
}

static uint32_t unknown_cmds;

/* Everything waiting in the receive FIFO. */
static void glink_rx(void)
{
    for (;;) {
        uint32_t avail = rx_avail();
        if (avail < 8) return;
        uint32_t m[2];
        rx_peek(m, 0, 8);
        uint16_t cmd = (uint16_t)m[0], p1 = (uint16_t)(m[0] >> 16);
        uint32_t p2 = m[1];
        switch (cmd) {
        case CMD_VERSION:
            rx_advance(8);
            features &= p2;
            tx_cmd(CMD_VERSION_ACK, 1, features);
            break;
        case CMD_VERSION_ACK:
            rx_advance(8);
            if (p1 == 0) { kprintf("glink: the modem refused version 1\n"); break; }
            if (p2 != features) { features &= p2; tx_cmd(CMD_VERSION, 1, features); break; }
            if (!link_version) kprintf("glink: link up (features %x)\n", features);
            link_version = 1;
            break;
        case CMD_OPEN: {
            uint32_t n = p2 & 0xffff, total = 8 + ((n + 7) & ~7u);
            if (avail < total) return;
            char name[33] = { 0 };
            rx_peek(name, 8, n < 32 ? n : 32);
            rx_advance(total);
            struct chan *c = chan_by_name(name);
            if (!c && nchans < MAX_CHANS) { c = &chans[nchans++]; memset(c, 0, sizeof *c); set_name(c, name); }
            if (!c) break;
            c->rcid = p1; c->remote_open = 1;
            kprintf("glink: the modem opens %s\n", name);
            if (c->on_rx) {                                     /* one of ours: open back */
                tx_cmd(CMD_OPEN_ACK, c->rcid, 0);
                open_local(c);
                if (c->acked && !c->up) chan_up(c);
            }
            break;
        }
        case CMD_OPEN_ACK: {
            rx_advance(8);
            struct chan *c = chan_by_lcid(p1);
            if (c) { c->acked = 1; if (c->remote_open && !c->up) chan_up(c); }
            break;
        }
        case CMD_CLOSE: {
            rx_advance(8);
            struct chan *c = chan_by_rcid(p1);
            tx_cmd(CMD_CLOSE_ACK, p1, 0);
            if (c) { kprintf("glink: the modem closes %s\n", c->name); c->remote_open = c->up = 0; c->ntheirs = 0; }
            break;
        }
        case CMD_CLOSE_ACK: case CMD_RX_INTENT_REQ_ACK: case CMD_SIGNALS:
            rx_advance(8);
            break;
        case CMD_INTENT: {
            uint32_t total = 8 + 8 * p2;
            if (avail < total) return;
            struct chan *c = chan_by_rcid(p1);
            for (uint32_t i = 0; i < p2; i++) {
                uint32_t pr[2];
                rx_peek(pr, 8 + 8 * i, 8);
                if (c && c->ntheirs < MAX_INTENTS) { c->theirs[c->ntheirs++] = (struct intent){ pr[1], pr[0], 0, NULL }; c->intent_asked = 0; }
            }
            rx_advance((total + 7) & ~7u);
            break;
        }
        case CMD_RX_DONE: case CMD_RX_DONE_W_REUSE: {
            rx_advance(8);
            struct chan *c = chan_by_rcid(p1);
            if (!c) break;
            for (int i = 0; i < c->ntheirs; i++)
                if (c->theirs[i].id == p2) {
                    if (cmd == CMD_RX_DONE_W_REUSE) c->theirs[i].used = 0;
                    else c->theirs[i] = c->theirs[--c->ntheirs];
                    break;
                }
            break;
        }
        case CMD_RX_INTENT_REQ: {
            rx_advance(8);
            struct chan *c = chan_by_rcid(p1);
            struct intent *in = c ? add_intent(c, p2) : NULL;
            if (in) advertise(c, in);
            if (c) tx_cmd(CMD_RX_INTENT_REQ_ACK, c->lcid, in != NULL);
            break;
        }
        case CMD_TX_DATA: case CMD_TX_DATA_CONT: {
            if (avail < 16) return;
            uint32_t h[4];
            rx_peek(h, 0, 16);
            uint32_t chunk = h[2], left = h[3];
            if (avail < 16 + chunk) return;
            struct chan *c = chan_by_rcid(p1);
            struct intent *in = NULL;
            for (int i = 0; c && i < c->nours; i++) if (c->ours[i].id == p2) in = &c->ours[i];
            if (in) {
                if (cmd == CMD_TX_DATA) in->used = 0;
                if (in->used + chunk <= in->size) { rx_peek(in->buf + in->used, 16, chunk); in->used += chunk; }
                if (!left) {
                    c->rx_count++;
                    if (c->on_rx) c->on_rx(c, in->buf, in->used);
                    in->used = 0;
                    tx_cmd(CMD_RX_DONE_W_REUSE, c->lcid, in->id);
                }
            }
            rx_advance((16 + chunk + 7) & ~7u);
            break;
        }
        case CMD_READ_NOTIF:
            rx_advance(8);
            kick(IPC_GLINK_MODEM);
            break;
        default:
            if (unknown_cmds++ < 8) kprintf("glink: unknown command %u (%x %x)\n", cmd, p1, p2);
            rx_advance(8);
            break;
        }
    }
}

/* The modem made its FIFO (480): the edge comes up, as Linux's
 * qcom_glink_smem_register (which runs after the modem started, too). */
static int glink_attach(void)
{
    size_t sz;
    if (!smem_get(MODEM_HOST, GLINK_FIFO_RX, NULL)) return -1;
    if (smem_alloc(MODEM_HOST, GLINK_DESC, 32) || smem_alloc(MODEM_HOST, GLINK_FIFO_TX, GLINK_FIFO_SIZE)) { kprintf("glink: cannot allocate our side in SMEM\n"); return -2; }
    desc = smem_get(MODEM_HOST, GLINK_DESC, &sz);
    if (!desc || sz != 32) { kprintf("glink: bad descriptor\n"); desc = NULL; return -2; }
    tx_fifo = smem_get(MODEM_HOST, GLINK_FIFO_TX, &sz); tx_len = (uint32_t)sz;
    rx_fifo = smem_get(MODEM_HOST, GLINK_FIFO_RX, &sz); rx_len = (uint32_t)sz;
    if (!tx_fifo || !rx_fifo) { desc = NULL; return -2; }
    desc[2] = 0;                                    /* our rx tail, our tx head */
    desc[1] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    kprintf("glink: edge to the modem (FIFOs %u/%u)\n", tx_len, rx_len);
    tx_cmd(CMD_VERSION, 1, features);
    return 0;
}

/* ---- QRTR --------------------------------------------------------------------------------- */

#define QRTR_NODE_LOCAL     1
#define QRTR_NODE_BCAST     0xffffffffu
#define QRTR_PORT_CTRL      0xfffffffeu

enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6,
       QRTR_RESUME_TX = 7, QRTR_EXIT = 8, QRTR_PING = 9, QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

static struct chan *ipcrtr;
static uint32_t remote_node = QRTR_NODE_BCAST;
static int hello_sent, hello_got;

struct qrtr_service { uint32_t service, instance, port; qrtr_handler_t handler; };
#define MAX_SERVICES 8
static struct qrtr_service local[MAX_SERVICES];
static int nlocal;

struct remote_service { uint32_t service, instance, node, port; };
#define MAX_REMOTE 64
static struct remote_service remote[MAX_REMOTE];
static int nremote;

static int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data, size_t len)
{
    if (!ipcrtr || !ipcrtr->up) return -1;
    size_t padded = (len + 3) & ~(size_t)3;
    uint8_t *pkt = kzalloc(32 + padded);
    if (!pkt) return -1;
    uint32_t h[8] = { 1, type, QRTR_NODE_LOCAL, src_port, 0, (uint32_t)len, dst_node, dst_port };
    memcpy(pkt, h, 32);
    if (len) memcpy(pkt + 32, data, len);
    int rc = glink_send(ipcrtr, pkt, 32 + padded);
    kfree(pkt);
    return rc;
}

int qrtr_sendto(uint32_t src_port, uint32_t node, uint32_t port, const void *data, size_t len)
{
    return qrtr_send(QRTR_DATA, src_port, node, port, data, len);
}

static void ctrl_send(uint32_t type, uint32_t node, const uint32_t *body, size_t words)
{
    uint32_t pkt[5] = { type, 0, 0, 0, 0 };                     /* struct qrtr_ctrl_pkt */
    for (size_t i = 0; i < words && i < 4; i++) pkt[1 + i] = body[i];
    qrtr_send(type, QRTR_PORT_CTRL, node, QRTR_PORT_CTRL, pkt, 20);
}

static void announce(uint32_t node)
{
    for (int i = 0; i < nlocal; i++) {
        uint32_t b[4] = { local[i].service, local[i].instance, QRTR_NODE_LOCAL, local[i].port };
        ctrl_send(QRTR_NEW_SERVER, node, b, 4);
    }
}

int qrtr_add_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port, qrtr_handler_t handler)
{
    if (nlocal >= MAX_SERVICES) return -1;
    local[nlocal++] = (struct qrtr_service){ service, version | instance << 8, port, handler };
    if (hello_got) {
        uint32_t b[4] = { service, version | instance << 8, QRTR_NODE_LOCAL, port };
        ctrl_send(QRTR_NEW_SERVER, remote_node, b, 4);
    }
    return 0;
}

static void ipcrtr_up(struct chan *c)
{
    (void)c;
    ctrl_send(QRTR_HELLO, QRTR_NODE_BCAST, NULL, 0);
    hello_sent = 1;
}

static void ipcrtr_rx(struct chan *c, const uint8_t *p, size_t len)
{
    (void)c;
    uint32_t type, src_node, src_port, confirm, size, dst_node, dst_port;
    size_t hlen;
    if (len < 16) return;
    if (p[0] == 1) {                                    /* v1: eight little-endian words */
        if (len < 32) return;
        uint32_t h[8];
        memcpy(h, p, 32);
        type = h[1]; src_node = h[2]; src_port = h[3]; confirm = h[4]; size = h[5]; dst_node = h[6]; dst_port = h[7];
        hlen = 32;
    } else if (p[0] == 3) {                             /* v2: bytes and halfwords */
        uint16_t s[4];
        memcpy(&size, p + 4, 4);
        memcpy(s, p + 8, 8);
        type = p[1]; confirm = p[2] & 1;
        src_node = s[0]; src_port = s[1] == 0xfffe ? QRTR_PORT_CTRL : s[1];
        dst_node = s[2]; dst_port = s[3] == 0xfffe ? QRTR_PORT_CTRL : s[3];
        hlen = 16 + p[3];
    } else return;
    if (hlen + size > len) return;
    const uint8_t *data = p + hlen;
    if (dst_port == 0xffff) dst_port = QRTR_PORT_CTRL;
    (void)dst_node;

    if (type == QRTR_DATA) {
        if (confirm) {                                  /* flow control: let it send more */
            uint32_t pkt[5] = { QRTR_RESUME_TX, QRTR_NODE_LOCAL, dst_port, 0, 0 };
            qrtr_send(QRTR_RESUME_TX, dst_port, src_node, src_port, pkt, 20);
        }
        for (int i = 0; i < nlocal; i++)
            if (local[i].port == dst_port) { local[i].handler(src_node, src_port, data, size); return; }
        return;
    }
    if (size < 4) return;
    uint32_t w[5] = { 0 };
    memcpy(w, data, size < 20 ? size : 20);
    switch (type) {
    case QRTR_HELLO:
        remote_node = src_node;
        if (!hello_got) kprintf("qrtr: hello from node %u\n", src_node);
        hello_got = 1;
        if (!hello_sent) { ctrl_send(QRTR_HELLO, src_node, NULL, 0); hello_sent = 1; }
        announce(src_node);
        break;
    case QRTR_NEW_SERVER:
        if (!w[1]) break;
        for (int i = 0; i < nremote; i++)
            if (remote[i].service == w[1] && remote[i].instance == w[2] && remote[i].node == w[3]) { remote[i].port = w[4]; return; }
        if (nremote < MAX_REMOTE) remote[nremote++] = (struct remote_service){ w[1], w[2], w[3], w[4] };
        break;
    case QRTR_DEL_SERVER:
        for (int i = 0; i < nremote; i++)
            if (remote[i].service == w[1] && remote[i].node == w[3] && remote[i].port == w[4]) { remote[i] = remote[--nremote]; break; }
        break;
    case QRTR_BYE:
        kprintf("qrtr: bye from node %u\n", src_node);
        nremote = 0;
        break;
    default:
        break;
    }
}

/* A service the modem offers (QMI service id), once it said so: its node
 * and port, or -1. */
int qrtr_lookup(uint32_t service, uint32_t *node, uint32_t *port)
{
    for (int i = 0; i < nremote; i++)
        if (remote[i].service == service) { *node = remote[i].node; *port = remote[i].port; return 0; }
    return -1;
}

/* ---- the thread, and /dev/qrtr ---------------------------------------------------------------- */

static int running;
static int64_t last_bits = -2;

static void ipc_thread(void *arg)
{
    (void)arg;
    uint64_t t0 = timer_ms();
    int attached = 0;
    for (;;) {
        if (!attached) {
            int rc = glink_attach();
            if (rc == 0) attached = 1;
            else if (rc == -2 || timer_ms() - t0 > 30000) { if (rc != -2) kprintf("glink: the modem made no FIFO in 30 s\n"); attached = -1; }
        }
        if (attached == 1) { glink_rx(); flush_queues(); }
        int64_t bits = qcom_smp2p_in("slave-kernel");
        if (bits != last_bits) {
            if (bits >= 0) kprintf("smp2p: the modem's bits %llx%s%s%s%s\n", (unsigned long long)bits, bits & 1 ? " FATAL" : "",
                                   bits & 2 ? " ready" : "", bits & 4 ? " handover" : "", bits & 8 ? " stop-ack" : "");
            last_bits = bits;
        }
        task_sleep_ms(2);
    }
}

void qcom_ipc_start(void)
{
    if (__atomic_exchange_n(&running, 1, __ATOMIC_SEQ_CST)) return;
    ipcrtr = glink_channel("IPCRTR", ipcrtr_up, ipcrtr_rx);
    task_create("qcom-ipc", ipc_thread, NULL);
}

static char report[2048];

static size_t make_report(void)
{
    size_t n = 0;
#define P(...) do { if (n < sizeof report) n += (size_t)ksnprintf(report + n, sizeof report - n, __VA_ARGS__); } while (0)
    P("glink: %s, features %x\n", !desc ? "not attached" : link_version ? "up" : "waiting for the modem's version ack", features);
    if (desc) P("  fifo tx %u/%u rx %u/%u\n", desc[1], desc[0], desc[3], desc[2]);
    for (int i = 0; i < nchans; i++)
        P("  channel %s: %s, ours %u its %u, %d/%d intents, %u in %u out%s\n", chans[i].name, chans[i].up ? "open" : chans[i].remote_open ? "opened by the modem" : "closed",
          chans[i].lcid, chans[i].rcid, chans[i].nours, chans[i].ntheirs, chans[i].rx_count, chans[i].tx_count, chans[i].queue ? ", queued" : "");
    P("qrtr: %s, the modem is node %d\n", hello_got ? "hello exchanged" : hello_sent ? "hello sent" : "no hello yet", (int)remote_node);
    for (int i = 0; i < nlocal; i++) P("  ours: service %u (instance %x) on port %u\n", local[i].service, local[i].instance, local[i].port);
    for (int i = 0; i < nremote; i++) P("  the modem's: service %u instance %x at %u:%u\n", remote[i].service, remote[i].instance, remote[i].node, remote[i].port);
#undef P
    if (n >= sizeof report) n = sizeof report - 1;
    return n;
}

static long qrtr_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) make_report();
    size_t n = strlen(report);
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static const struct dev_ops qrtr_ops = { .read = qrtr_read };

void qcom_ipc_init(void) { vfs_mkdev("/dev/qrtr", &qrtr_ops, NULL); }
