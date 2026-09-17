/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* TCP: an RFC 793 state machine with a fixed-size send/receive buffer per
 * connection, out-of-order reassembly with selective acknowledgements
 * (RFC 2018) in both directions, delayed ACKs (RFC 1122), retransmission
 * with exponential backoff that skips SACKed data, fast retransmit on three
 * duplicate ACKs, and a small fixed congestion window. No timestamps, no
 * window scaling, no Nagle. */
#include "net/net.h"
#include "mm/heap.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"

struct tcp_hdr {
    uint16_t src, dst;
    uint32_t seq, ack;
    uint8_t  off, flags;
    uint16_t wnd, csum, urg;
} __attribute__((packed));

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

#define RTO_MIN_MS   300
#define RTO_MAX_MS   8000
#define MAX_RETRIES  8
#define TIME_WAIT_MS 1000
#define CWND_SEGS    16
#define DELACK_MS    40
#define OOO_MAX      TCP_BUF_SIZE

static uint32_t tcp_iss(void)
{
    static uint32_t counter;
    counter += 64000;
    return (uint32_t)(timer_ms() * 1000) + counter;
}

uint16_t tcp_ephemeral_port(void)
{
    static uint16_t next = 49152;
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t port = next++;
        if (next == 0) next = 49152;
        int used = 0;
        for (struct socket *s = tcp_sockets; s; s = s->next)
            if (s->local_port == htons(port)) { used = 1; break; }
        if (!used)
            return htons(port);
    }
    return 0;
}

struct socket *tcp_lookup(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport)
{
    struct socket *listener = NULL;
    for (struct socket *s = tcp_sockets; s; s = s->next) {
        if (s->local_port != lport)
            continue;
        if (s->tcp && s->tcp->state != TCP_LISTEN && s->tcp->state != TCP_CLOSED &&
            s->remote_ip == rip && s->remote_port == rport && (s->local_ip == lip || s->local_ip == 0))
            return s;
        if (s->listening && (s->local_ip == 0 || s->local_ip == lip))
            listener = s;
    }
    return listener;
}

static struct tcp_pcb *pcb_alloc(void)
{
    struct tcp_pcb *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->sndbuf = kmalloc(TCP_BUF_SIZE);
    t->rcvbuf = kmalloc(TCP_BUF_SIZE);
    if (!t->sndbuf || !t->rcvbuf) {
        kfree(t->sndbuf);
        kfree(t->rcvbuf);
        kfree(t);
        return NULL;
    }
    t->mss = TCP_MSS;
    t->rto_ms = RTO_MIN_MS;
    return t;
}

static void ooo_flush(struct tcp_pcb *t)
{
    while (t->ooo) {
        struct tcp_ooo *o = t->ooo;
        t->ooo = o->next;
        kfree(o->data);
        kfree(o);
    }
    t->ooo_bytes = 0;
}

static void pcb_free(struct tcp_pcb *t)
{
    ooo_flush(t);
    kfree(t->sndbuf);
    kfree(t->rcvbuf);
    kfree(t);
}

static uint32_t rcv_space(struct tcp_pcb *t) { return TCP_BUF_SIZE - t->rcv_len; }

/* Build and send one segment. `data` may be NULL; `opts` is optlen bytes
 * of TCP options, optlen a multiple of 4 (at most 40). */
static int tcp_emit(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                    uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                    const void *data, uint32_t len, const uint8_t *opts, uint32_t optlen)
{
    struct pkt *p = pkt_alloc();
    if (!p)
        return -ENOMEM;
    if (len) {
        memcpy(pkt_data(p), data, len);
        p->len = len;
    }
    struct tcp_hdr *h = pkt_push(p, sizeof(*h) + optlen);
    h->src = sport;
    h->dst = dport;
    h->seq = htonl(seq);
    h->ack = htonl(ack);
    h->off = (uint8_t)(((sizeof(*h) + optlen) / 4) << 4);
    h->flags = flags;
    h->wnd = htons(wnd);
    h->csum = 0;
    h->urg = 0;
    if (optlen)
        memcpy(h + 1, opts, optlen);
    h->csum = ip_pseudo_checksum(src, dst, IPPROTO_TCP, h, p->len);
    return ip_send(src, dst, IPPROTO_TCP, p);
}

/* Options for a SYN: MSS, and SACK-permitted. */
static uint32_t syn_options(struct tcp_pcb *t, uint8_t *o)
{
    o[0] = 2; o[1] = 4; o[2] = (uint8_t)(t->mss >> 8); o[3] = (uint8_t)t->mss;
    o[4] = 1; o[5] = 1; o[6] = 4; o[7] = 2;             /* NOP NOP SACK-permitted */
    return 8;
}

/* SACK blocks describing the out-of-order data we hold, newest first
 * (t->ooo is kept sorted; the newest block is remembered separately). */
static uint32_t sack_options(struct tcp_pcb *t, uint8_t *o, uint32_t newest_seq)
{
    if (!t->sack_ok || !t->ooo)
        return 0;
    uint32_t blocks[4][2];
    int n = 0;
    for (int pass = 0; pass < 2 && n < 4; pass++)
        for (struct tcp_ooo *x = t->ooo; x && n < 4; x = x->next) {
            int is_newest = SEQ_LEQ(x->seq, newest_seq) && SEQ_LT(newest_seq, x->seq + x->len);
            if ((pass == 0) != is_newest)
                continue;
            blocks[n][0] = x->seq;
            blocks[n][1] = x->seq + x->len;
            n++;
        }
    o[0] = 1; o[1] = 1; o[2] = 5; o[3] = (uint8_t)(2 + 8 * n);
    for (int i = 0; i < n; i++) {
        uint32_t a = htonl(blocks[i][0]), b = htonl(blocks[i][1]);
        memcpy(o + 4 + 8 * i, &a, 4);
        memcpy(o + 8 + 8 * i, &b, 4);
    }
    return 4 + 8 * (uint32_t)n;
}

static int tcp_send_seg(struct socket *s, uint32_t seq, uint8_t flags, const void *data, uint32_t len, int with_sack)
{
    struct tcp_pcb *t = s->tcp;
    uint32_t wnd = rcv_space(t);
    if (wnd > 65535) wnd = 65535;
    uint8_t opts[40];
    uint32_t optlen = 0;
    if (flags & TH_SYN)
        optlen = syn_options(t, opts);
    else if (with_sack)
        optlen = sack_options(t, opts, t->ooo ? t->ooo->seq : 0);
    if (flags & TH_ACK) {           /* every ACK we send covers what was pending */
        t->ack_pending = 0;
        t->ack_segs = 0;
        t->delack_at = 0;
    }
    return tcp_emit(s->local_ip, s->remote_ip, s->local_port, s->remote_port,
                    seq, t->rcv_nxt, flags, (uint16_t)wnd, data, len, opts, optlen);
}

static void tcp_send_ack(struct socket *s)
{
    tcp_send_seg(s, s->tcp->snd_nxt, TH_ACK, NULL, 0, 1);
}

/* Delayed ACK: acknowledge now after two full segments, otherwise let the
 * timer (or the next data segment we send) carry it. */
static void tcp_ack_later(struct socket *s, uint32_t datalen)
{
    struct tcp_pcb *t = s->tcp;
    t->ack_pending = 1;
    if (datalen >= t->mss)
        t->ack_segs++;
    if (t->ack_segs >= 2) {
        tcp_send_ack(s);
        return;
    }
    if (!t->delack_at)
        t->delack_at = timer_ms() + DELACK_MS;
}

/* Is [seq, seq + len) entirely covered by a SACKed range? Returns the end of
 * that range so the sender can jump past it, or 0. */
static uint32_t sacked_through(struct tcp_pcb *t, uint32_t seq)
{
    for (int i = 0; i < t->nsacked; i++)
        if (SEQ_LEQ(t->sacked[i].start, seq) && SEQ_LT(seq, t->sacked[i].end))
            return t->sacked[i].end;
    return 0;
}

/* The first SACKed range starting inside [seq, seq + len): its start, else 0. */
static uint32_t next_sack_start(struct tcp_pcb *t, uint32_t seq, uint32_t len)
{
    uint32_t best = 0;
    int found = 0;
    for (int i = 0; i < t->nsacked; i++)
        if (SEQ_GT(t->sacked[i].start, seq) && SEQ_LT(t->sacked[i].start, seq + len) && (!found || SEQ_LT(t->sacked[i].start, best))) {
            best = t->sacked[i].start;
            found = 1;
        }
    return found ? best : 0;
}

static void arm_rto(struct tcp_pcb *t)
{
    if (!t->rto_at)
        t->rto_at = timer_ms() + t->rto_ms;
}

/* Leave the connection for good: the protocol's reference goes away. */
static void tcp_set_closed(struct socket *s, int err)
{
    struct tcp_pcb *t = s->tcp;
    if (t->state == TCP_CLOSED)
        return;
    t->state = TCP_CLOSED;
    t->rto_at = 0;
    if (err && !s->err)
        s->err = err;
    if (s->listener) {              /* never accepted: drop it from the backlog */
        struct socket *l = s->listener;
        struct socket **pp = &l->accept_head;
        while (*pp && *pp != s)
            pp = &(*pp)->accept_next;
        if (*pp) {
            *pp = s->accept_next;
            if (l->accept_tail == s) {
                l->accept_tail = NULL;
                for (struct socket *x = l->accept_head; x; x = x->accept_next)
                    l->accept_tail = x;
            }
            l->accept_count--;
        }
        s->listener = NULL;
        waitqueue_wake_all(&s->wq);
        sock_put(s);
        return;
    }
    waitqueue_wake_all(&s->wq);
    sock_put(s);
}

/* Send whatever the window allows: SYN, data, FIN. */
static void tcp_output(struct socket *s)
{
    struct tcp_pcb *t = s->tcp;
    switch (t->state) {
    case TCP_SYN_SENT:
        if (t->snd_nxt == t->iss) {
            tcp_send_seg(s, t->iss, TH_SYN, NULL, 0, 0);
            t->snd_nxt = t->iss + 1;
            arm_rto(t);
        }
        return;
    case TCP_SYN_RCVD:
        if (t->snd_nxt == t->iss) {
            tcp_send_seg(s, t->iss, TH_SYN | TH_ACK, NULL, 0, 0);
            t->snd_nxt = t->iss + 1;
            arm_rto(t);
        }
        return;
    case TCP_ESTABLISHED: case TCP_CLOSE_WAIT: case TCP_FIN_WAIT_1: case TCP_CLOSING: case TCP_LAST_ACK:
        break;
    default:
        return;
    }
    uint32_t wnd = t->snd_wnd < (uint32_t)CWND_SEGS * t->mss ? t->snd_wnd : (uint32_t)CWND_SEGS * t->mss;
    if (wnd == 0 && t->snd_nxt == t->snd_una && t->snd_len > 0)
        wnd = 1;                    /* zero window: probe with one byte */
    for (;;) {
        uint32_t inflight = t->snd_nxt - t->snd_una;
        if (inflight >= t->snd_len || inflight >= wnd)
            break;
        /* retransmitting: the peer already holds SACKed ranges, jump over them */
        uint32_t sacked_end = sacked_through(t, t->snd_nxt);
        if (sacked_end) {
            uint32_t limit = t->snd_una + t->snd_len;
            t->snd_nxt = SEQ_LT(sacked_end, limit) ? sacked_end : limit;
            continue;
        }
        uint32_t n = t->snd_len - inflight;
        if (n > t->mss) n = t->mss;
        if (n > wnd - inflight) n = wnd - inflight;
        uint32_t hole_end = next_sack_start(t, t->snd_nxt, n);
        if (hole_end) n = hole_end - t->snd_nxt;
        /* in fast recovery only the holes below `recover` are resent; new
         * data waits until the ACK clock says the loss is repaired */
        if (t->recover && SEQ_GEQ(t->snd_nxt, t->recover))
            break;
        uint8_t flags = TH_ACK;
        if (inflight + n == t->snd_len) flags |= TH_PSH;
        if (tcp_send_seg(s, t->snd_nxt, flags, t->sndbuf + inflight, n, 0) < 0)
            break;
        t->snd_nxt += n;
        arm_rto(t);
    }
    if (t->fin_queued && !t->fin_sent && t->snd_nxt == t->snd_una + t->snd_len && !(t->recover && SEQ_GEQ(t->snd_nxt, t->recover))) {
        tcp_send_seg(s, t->snd_nxt, TH_FIN | TH_ACK, NULL, 0, 0);
        t->snd_nxt++;
        t->fin_sent = 1;
        arm_rto(t);
    }
    if (t->recover && SEQ_GEQ(t->snd_nxt, t->recover))
        t->recover = 0;             /* every hole has been resent: back to normal sending */
}

/* fin_sent and the FIN's own sequence number: the FIN always sits right
 * after the last byte of data, so it is acked once snd_una passes it. */
static int fin_acked(struct tcp_pcb *t)
{
    return t->fin_sent && t->snd_una == t->snd_nxt;
}

static void tcp_abort(struct socket *s, int err)
{
    if (s->tcp->state != TCP_CLOSED && s->tcp->state != TCP_LISTEN)
        tcp_send_seg(s, s->tcp->snd_nxt, TH_RST | TH_ACK, NULL, 0, 0);
    tcp_set_closed(s, err);
}

static void tcp_rst_reply(struct pkt *p, struct tcp_hdr *th, uint32_t datalen)
{
    if (th->flags & TH_RST)
        return;
    if (th->flags & TH_ACK)
        tcp_emit(p->dst_ip, p->src_ip, th->dst, th->src, ntohl(th->ack), 0, TH_RST, 0, NULL, 0, NULL, 0);
    else
        tcp_emit(p->dst_ip, p->src_ip, th->dst, th->src, 0,
                 ntohl(th->seq) + datalen + ((th->flags & TH_SYN) ? 1 : 0) + ((th->flags & TH_FIN) ? 1 : 0),
                 TH_RST | TH_ACK, 0, NULL, 0, NULL, 0);
}

struct tcp_opts {
    uint16_t mss;
    int sack_ok;
    int nsack;
    uint32_t sack[4][2];
};

static void parse_opts(struct tcp_hdr *th, struct tcp_opts *o)
{
    memset(o, 0, sizeof(*o));
    uint8_t *b = (uint8_t *)(th + 1);
    uint32_t n = (uint32_t)((th->off >> 4) * 4) - sizeof(*th);
    for (uint32_t i = 0; i < n;) {
        if (b[i] == 0) break;
        if (b[i] == 1) { i++; continue; }
        if (i + 1 >= n || b[i + 1] < 2 || i + b[i + 1] > n) break;
        uint8_t kind = b[i], len = b[i + 1];
        if (kind == 2 && len == 4)
            o->mss = (uint16_t)(b[i + 2] << 8 | b[i + 3]);
        else if (kind == 4 && len == 2)
            o->sack_ok = 1;
        else if (kind == 5 && len >= 10) {
            for (uint32_t k = 2; k + 8 <= len && o->nsack < 4; k += 8) {
                uint32_t a, e;
                memcpy(&a, b + i + k, 4);
                memcpy(&e, b + i + k + 4, 4);
                o->sack[o->nsack][0] = ntohl(a);
                o->sack[o->nsack][1] = ntohl(e);
                o->nsack++;
            }
        }
        i += len;
    }
}

/* Merge the peer's SACK blocks into t->sacked (kept within [snd_una, snd_nxt)). */
static void note_sacked(struct tcp_pcb *t, const struct tcp_opts *o)
{
    for (int i = 0; i < o->nsack; i++) {
        uint32_t a = o->sack[i][0], e = o->sack[i][1];
        if (SEQ_GEQ(a, e) || SEQ_LT(a, t->snd_una) || SEQ_GT(e, t->snd_nxt))
            continue;
        /* absorb overlapping ranges */
        for (int k = 0; k < t->nsacked; k++) {
            if (SEQ_LEQ(t->sacked[k].start, e) && SEQ_LEQ(a, t->sacked[k].end)) {
                if (SEQ_LT(t->sacked[k].start, a)) a = t->sacked[k].start;
                if (SEQ_GT(t->sacked[k].end, e)) e = t->sacked[k].end;
                t->sacked[k] = t->sacked[--t->nsacked];
                k = -1;
            }
        }
        if (t->nsacked < TCP_MAX_SACK) {
            t->sacked[t->nsacked].start = a;
            t->sacked[t->nsacked].end = e;
            t->nsacked++;
        }
    }
}

/* Drop SACK ranges that a cumulative ACK has made irrelevant. */
static void trim_sacked(struct tcp_pcb *t)
{
    for (int k = 0; k < t->nsacked;) {
        if (SEQ_LEQ(t->sacked[k].end, t->snd_una))
            t->sacked[k] = t->sacked[--t->nsacked];
        else {
            if (SEQ_LT(t->sacked[k].start, t->snd_una)) t->sacked[k].start = t->snd_una;
            k++;
        }
    }
}

/* Out-of-order data: park it until the hole before it is filled. Segments
 * are stored trimmed against what we already hold, so the queue never
 * overlaps itself. */
static void ooo_insert(struct tcp_pcb *t, uint32_t seq, const uint8_t *data, uint32_t len)
{
    struct tcp_ooo **pp = &t->ooo;
    while (*pp && SEQ_LT((*pp)->seq + (*pp)->len, seq + 1))
        pp = &(*pp)->next;
    /* trim the front against the previous segment (pp points past it) */
    if (*pp && SEQ_LEQ((*pp)->seq, seq)) {       /* starts inside an existing one */
        uint32_t covered = (*pp)->seq + (*pp)->len - seq;
        if (covered >= len) return;
        data += covered; seq += covered; len -= covered;
        pp = &(*pp)->next;
    }
    /* trim the back against the next segment */
    if (*pp && SEQ_LT((*pp)->seq, seq + len))
        len = (*pp)->seq - seq;
    if (len == 0 || t->ooo_bytes + len > OOO_MAX)
        return;
    struct tcp_ooo *o = kmalloc(sizeof(*o));
    if (!o) return;
    o->data = kmalloc(len);
    if (!o->data) { kfree(o); return; }
    memcpy(o->data, data, len);
    o->seq = seq;
    o->len = len;
    o->next = *pp;
    *pp = o;
    t->ooo_bytes += len;
}

/* Append in-order bytes to the receive ring (as many as fit). */
static uint32_t rcv_append(struct tcp_pcb *t, const uint8_t *data, uint32_t len)
{
    uint32_t space = rcv_space(t);
    uint32_t n = len < space ? len : space;
    uint32_t tail = (t->rcv_head + t->rcv_len) % TCP_BUF_SIZE;
    uint32_t first = TCP_BUF_SIZE - tail < n ? TCP_BUF_SIZE - tail : n;
    memcpy(t->rcvbuf + tail, data, first);
    memcpy(t->rcvbuf, data + first, n - first);
    t->rcv_len += n;
    t->rcv_nxt += n;
    return n;
}

/* After rcv_nxt moved: pull now-contiguous out-of-order segments in. */
static void ooo_drain(struct tcp_pcb *t)
{
    while (t->ooo && SEQ_LEQ(t->ooo->seq, t->rcv_nxt)) {
        struct tcp_ooo *o = t->ooo;
        uint32_t skip = t->rcv_nxt - o->seq;
        if (skip < o->len)
            rcv_append(t, o->data + skip, o->len - skip);   /* always fits: it was inside the window */
        t->ooo = o->next;
        t->ooo_bytes -= o->len;
        kfree(o->data);
        kfree(o);
    }
}

static void tcp_passive_open(struct socket *l, struct pkt *p, struct tcp_hdr *th, const struct tcp_opts *o)
{
    if (l->accept_count >= l->backlog)
        return;
    struct socket *c = sock_create(SOCK_STREAM, IPPROTO_TCP);
    if (!c)
        return;
    c->tcp = pcb_alloc();
    if (!c->tcp) {
        sock_put(c);
        return;
    }
    c->local_ip = p->dst_ip;
    c->local_port = th->dst;
    c->remote_ip = p->src_ip;
    c->remote_port = th->src;
    c->bound = c->connected = 1;
    c->listener = l;
    c->accept_next = NULL;
    struct tcp_pcb *t = c->tcp;
    t->state = TCP_SYN_RCVD;
    t->irs = ntohl(th->seq);
    t->rcv_nxt = t->irs + 1;
    t->iss = tcp_iss();
    t->snd_una = t->snd_nxt = t->iss;
    t->snd_wnd = ntohs(th->wnd);
    if (o->mss && o->mss < t->mss) t->mss = o->mss;
    t->sack_ok = o->sack_ok;
    /* in the listener's backlog from now on; moved to the ready queue when established */
    if (l->accept_tail) l->accept_tail->accept_next = c; else l->accept_head = c;
    l->accept_tail = c;
    l->accept_count++;
    sock_link(c);                   /* the protocol's reference: sock_create gave us one */
    tcp_output(c);
}

void tcp_rx(struct pkt *p)
{
    struct tcp_hdr *th = (void *)pkt_data(p);
    if (p->len < sizeof(*th))
        goto out;
    uint32_t hlen = (uint32_t)(th->off >> 4) * 4;
    if (hlen < sizeof(*th) || hlen > p->len)
        goto out;
    if (ip_pseudo_checksum(p->src_ip, p->dst_ip, IPPROTO_TCP, th, p->len) != 0)
        goto out;
    uint32_t datalen = p->len - hlen;
    uint8_t *data = pkt_data(p) + hlen;
    uint32_t seq = ntohl(th->seq), ack = ntohl(th->ack);
    uint8_t flags = th->flags;
    struct tcp_opts opts;
    parse_opts(th, &opts);

    struct socket *s = tcp_lookup(p->dst_ip, th->dst, p->src_ip, th->src);
    if (!s || !s->tcp) {
        tcp_rst_reply(p, th, datalen);
        goto out;
    }
    struct tcp_pcb *t = s->tcp;

    switch (t->state) {
    case TCP_LISTEN:
        if (flags & TH_RST) break;
        if (flags & TH_ACK) { tcp_rst_reply(p, th, datalen); break; }
        if (flags & TH_SYN) tcp_passive_open(s, p, th, &opts);
        break;

    case TCP_SYN_SENT:
        if ((flags & TH_ACK) && (SEQ_LEQ(ack, t->iss) || SEQ_GT(ack, t->snd_nxt))) {
            tcp_rst_reply(p, th, datalen);
            break;
        }
        if (flags & TH_RST) {
            if (flags & TH_ACK) tcp_set_closed(s, ECONNREFUSED);
            break;
        }
        if (!(flags & TH_SYN)) break;
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        if (opts.mss && opts.mss < t->mss) t->mss = opts.mss;
        t->sack_ok = opts.sack_ok;
        if (flags & TH_ACK) {
            t->snd_una = ack;
            t->snd_wnd = ntohs(th->wnd);
            t->snd_wl1 = seq; t->snd_wl2 = ack;
            t->rto_at = 0;
            t->rto_ms = RTO_MIN_MS;
            t->retries = 0;
            t->state = TCP_ESTABLISHED;
            s->connected = 1;
            tcp_send_ack(s);
            waitqueue_wake_all(&s->wq);
        } else {                    /* simultaneous open */
            t->state = TCP_SYN_RCVD;
            t->snd_nxt = t->iss;    /* resend as SYN|ACK */
            tcp_output(s);
        }
        break;

    default: {
        /* Sequence check: the segment must overlap our window (or be a pure ack at rcv_nxt). */
        uint32_t wnd = rcv_space(t);
        int ok = datalen == 0 ? (wnd == 0 ? seq == t->rcv_nxt : (SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd)) || seq == t->rcv_nxt)
                              : (wnd > 0 && (SEQ_LT(seq, t->rcv_nxt + wnd) && SEQ_GEQ(seq + datalen - 1, t->rcv_nxt)));
        if (!ok) {
            if (!(flags & TH_RST)) tcp_send_ack(s);
            break;
        }
        if (flags & TH_RST) {
            tcp_set_closed(s, t->state == TCP_SYN_RCVD ? ECONNREFUSED : ECONNRESET);
            break;
        }
        if (flags & TH_SYN) {       /* SYN inside the window: fatal */
            tcp_abort(s, ECONNRESET);
            break;
        }
        if (!(flags & TH_ACK))
            break;

        /* ACK processing */
        if (t->state == TCP_SYN_RCVD) {
            if (SEQ_LT(ack, t->snd_una) || SEQ_GT(ack, t->snd_nxt)) {
                tcp_rst_reply(p, th, datalen);
                break;
            }
            t->snd_una = ack;
            t->snd_wnd = ntohs(th->wnd);
            t->snd_wl1 = seq; t->snd_wl2 = ack;
            t->rto_at = 0;
            t->rto_ms = RTO_MIN_MS;
            t->retries = 0;
            t->state = TCP_ESTABLISHED;
            if (s->listener)
                waitqueue_wake_all(&s->listener->wq);
        } else if (SEQ_GT(ack, t->snd_nxt)) {
            tcp_send_ack(s);
            break;
        } else if (SEQ_GT(ack, t->snd_una)) {
            uint32_t acked = ack - t->snd_una;
            uint32_t data_acked = acked < t->snd_len ? acked : t->snd_len;
            if (data_acked) {
                memmove(t->sndbuf, t->sndbuf + data_acked, t->snd_len - data_acked);
                t->snd_len -= data_acked;
            }
            t->snd_una = ack;
            if (t->recover && SEQ_GEQ(ack, t->recover))
                t->recover = 0;                 /* the loss is repaired */
            if (SEQ_LT(t->snd_nxt, t->snd_una))
                t->snd_nxt = t->snd_una;
            trim_sacked(t);
            note_sacked(t, &opts);
            t->retries = 0;
            t->rto_ms = RTO_MIN_MS;
            t->rto_at = t->snd_una == t->snd_nxt ? 0 : timer_ms() + t->rto_ms;
            t->dup_acks = 0;
            waitqueue_wake_all(&s->wq);         /* room in the send buffer */
        } else if (ack == t->snd_una && datalen == 0 && t->snd_nxt != t->snd_una && ntohs(th->wnd) == t->snd_wnd) {
            int before = t->nsacked;
            note_sacked(t, &opts);
            if (++t->dup_acks == 3) {
                /* fast retransmit: resend the holes below what the peer has,
                 * skipping SACKed ranges; without SACK this is go-back-N */
                t->recover = t->snd_nxt;
                t->snd_nxt = t->snd_una;
                t->fin_sent = 0;
                tcp_output(s);
            } else if (t->dup_acks > 3 && t->nsacked != before) {
                tcp_output(s);      /* new SACK information: send more of what's missing */
            }
        }
        /* window update */
        if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
            t->snd_wnd = ntohs(th->wnd);
            t->snd_wl1 = seq;
            t->snd_wl2 = ack;
        }
        switch (t->state) {
        case TCP_FIN_WAIT_1:
            if (fin_acked(t)) t->state = TCP_FIN_WAIT_2;
            break;
        case TCP_CLOSING:
            if (fin_acked(t)) { t->state = TCP_TIME_WAIT; t->timewait_at = timer_ms() + TIME_WAIT_MS; }
            break;
        case TCP_LAST_ACK:
            if (fin_acked(t)) { tcp_set_closed(s, 0); goto out; }
            break;
        case TCP_TIME_WAIT:
            t->timewait_at = timer_ms() + TIME_WAIT_MS;
            break;
        default: break;
        }

        /* data: in-order bytes go to the ring, out-of-order ones wait in the
         * reassembly queue and are reported back with SACK; both cases ACK,
         * only the in-order case may delay it */
        int ack_now = 0, ack_delayed = 0;
        uint32_t fin_seq = seq + datalen;       /* where a FIN would sit */
        if (datalen && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)) {
            int old = 0;
            if (SEQ_LT(seq, t->rcv_nxt)) {      /* partially old: skip the known prefix */
                uint32_t skip = t->rcv_nxt - seq;
                data += skip; datalen -= skip; seq += skip;
                old = 1;
            }
            if (seq == t->rcv_nxt && !s->shut_rd) {
                uint32_t n = rcv_append(t, data, datalen);
                if (n < datalen) flags &= ~TH_FIN;       /* FIN not yet reached */
                ooo_drain(t);
                if (t->ooo || old) ack_now = 1; else ack_delayed = 1;
                waitqueue_wake_all(&s->wq);
            } else if (seq == t->rcv_nxt) {
                t->rcv_nxt += datalen;                  /* reader gone: discard */
                ack_now = 1;
            } else {                                    /* a hole before it */
                if (!s->shut_rd && SEQ_LT(seq, t->rcv_nxt + wnd)) {
                    uint32_t n = datalen;
                    if (SEQ_GT(seq + n, t->rcv_nxt + wnd)) n = t->rcv_nxt + wnd - seq;
                    ooo_insert(t, seq, data, n);
                }
                flags &= ~TH_FIN;
                ack_now = 1;
            }
        } else if (datalen) {
            ack_now = 1;
            flags &= ~TH_FIN;
        }
        if ((flags & TH_FIN) && fin_seq == t->rcv_nxt && !t->fin_rcvd) {
            t->fin_rcvd = 1;
            t->rcv_nxt++;
            ack_now = 1;
            ack_delayed = 0;
            switch (t->state) {
            case TCP_SYN_RCVD: case TCP_ESTABLISHED: t->state = TCP_CLOSE_WAIT; break;
            case TCP_FIN_WAIT_1:
                if (fin_acked(t)) { t->state = TCP_TIME_WAIT; t->timewait_at = timer_ms() + TIME_WAIT_MS; }
                else t->state = TCP_CLOSING;
                break;
            case TCP_FIN_WAIT_2: t->state = TCP_TIME_WAIT; t->timewait_at = timer_ms() + TIME_WAIT_MS; break;
            default: break;
            }
            waitqueue_wake_all(&s->wq);
        }
        if (ack_now)
            tcp_send_ack(s);
        else if (ack_delayed)
            tcp_ack_later(s, datalen);
        tcp_output(s);              /* the window may have opened; data carries the ACK */
        break;
    }
    }
out:
    pkt_free(p);
}

/* ---- timers ------------------------------------------------------------------- */
void tcp_timer(uint64_t now)
{
    struct socket *next;
    for (struct socket *s = tcp_sockets; s; s = next) {
        next = s->next;
        struct tcp_pcb *t = s->tcp;
        if (!t)
            continue;
        if (t->state == TCP_TIME_WAIT && now >= t->timewait_at) {
            tcp_set_closed(s, 0);
            continue;
        }
        if (t->ack_pending && t->delack_at && now >= t->delack_at)
            tcp_send_ack(s);
        if (t->rto_at && now >= t->rto_at) {
            if (++t->retries > MAX_RETRIES) {
                tcp_abort(s, t->state == TCP_SYN_SENT ? ETIMEDOUT : ETIMEDOUT);
                continue;
            }
            t->rto_ms *= 2;
            if (t->rto_ms > RTO_MAX_MS) t->rto_ms = RTO_MAX_MS;
            t->rto_at = 0;
            if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD)
                t->snd_nxt = t->iss;
            else {
                t->snd_nxt = t->snd_una;
                t->fin_sent = 0;
                t->recover = 0;
            }
            tcp_output(s);
            if (!t->rto_at)
                t->rto_at = now + t->rto_ms;
        }
    }
}

/* ---- socket-layer entry points (net_lock held) -------------------------------- */
int tcp_connect(struct socket *s)
{
    if (s->tcp)
        return -EISCONN;
    s->tcp = pcb_alloc();
    if (!s->tcp)
        return -ENOMEM;
    struct tcp_pcb *t = s->tcp;
    t->iss = tcp_iss();
    t->snd_una = t->snd_nxt = t->iss;
    t->snd_wnd = t->mss;
    t->state = TCP_SYN_SENT;
    sock_get(s);                    /* the protocol's reference */
    sock_link(s);
    tcp_output(s);
    return 0;
}

int tcp_listen(struct socket *s)
{
    if (s->tcp)
        return -EINVAL;
    s->tcp = kzalloc(sizeof(*s->tcp));
    if (!s->tcp)
        return -ENOMEM;
    s->tcp->state = TCP_LISTEN;
    s->listening = 1;
    sock_get(s);
    sock_link(s);
    return 0;
}

int tcp_send(struct socket *s, const void *buf, size_t len)
{
    struct tcp_pcb *t = s->tcp;
    if (!t) return -ENOTCONN;
    if (s->shut_wr || t->fin_queued) return -EPIPE;
    if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) return -EAGAIN;
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) return s->err ? -s->err : -EPIPE;
    uint32_t space = TCP_BUF_SIZE - t->snd_len;
    if (space == 0)
        return -EAGAIN;
    uint32_t n = len < space ? (uint32_t)len : space;
    memcpy(t->sndbuf + t->snd_len, buf, n);
    t->snd_len += n;
    tcp_output(s);
    return (int)n;
}

int tcp_recv(struct socket *s, void *buf, size_t len, int peek)
{
    struct tcp_pcb *t = s->tcp;
    if (!t) return -ENOTCONN;
    if (t->rcv_len == 0) {
        if (t->state == TCP_CLOSED && s->err) { int e = s->err; if (!peek) s->err = 0; return -e; }
        if (t->fin_rcvd || t->state == TCP_CLOSED || s->shut_rd) return 0;
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) return -EAGAIN;
        return -EAGAIN;
    }
    uint32_t n = len < t->rcv_len ? (uint32_t)len : t->rcv_len;
    uint32_t first = TCP_BUF_SIZE - t->rcv_head < n ? TCP_BUF_SIZE - t->rcv_head : n;
    memcpy(buf, t->rcvbuf + t->rcv_head, first);
    memcpy((uint8_t *)buf + first, t->rcvbuf, n - first);
    if (!peek) {
        int was_full = rcv_space(t) < t->mss;
        t->rcv_head = (t->rcv_head + n) % TCP_BUF_SIZE;
        t->rcv_len -= n;
        if (was_full && t->state != TCP_CLOSED)
            tcp_send_ack(s);        /* window update */
    }
    return (int)n;
}

void tcp_shutdown_wr(struct socket *s)
{
    struct tcp_pcb *t = s->tcp;
    if (!t || t->fin_queued)
        return;
    switch (t->state) {
    case TCP_ESTABLISHED: t->state = TCP_FIN_WAIT_1; break;
    case TCP_CLOSE_WAIT:  t->state = TCP_LAST_ACK; break;
    case TCP_SYN_RCVD:    t->state = TCP_FIN_WAIT_1; break;
    default: return;
    }
    t->fin_queued = 1;
    tcp_output(s);
}

/* The file went away. */
void tcp_close(struct socket *s)
{
    struct tcp_pcb *t = s->tcp;
    if (!t)
        return;
    switch (t->state) {
    case TCP_LISTEN: {
        /* abort every embryonic/ready connection nobody will accept */
        struct socket *c = s->accept_head;
        while (c) {
            struct socket *n = c->accept_next;
            c->listener = NULL;
            tcp_abort(c, ECONNABORTED);
            c = n;
        }
        s->accept_head = s->accept_tail = NULL;
        s->accept_count = 0;
        tcp_set_closed(s, 0);
        return;
    }
    case TCP_SYN_SENT:
        tcp_set_closed(s, 0);
        return;
    case TCP_SYN_RCVD: case TCP_ESTABLISHED: case TCP_CLOSE_WAIT:
        if (t->rcv_len)             /* unread data: reset rather than pretend a clean close */
            tcp_abort(s, 0);
        else
            tcp_shutdown_wr(s);
        return;
    default:
        return;                     /* already closing; the state machine finishes */
    }
}

int tcp_poll(struct socket *s)
{
    struct tcp_pcb *t = s->tcp;
    int mask = 0;
    if (!t)
        return POLLOUT | POLLHUP;   /* unconnected stream socket */
    if (t->state == TCP_LISTEN) {
        for (struct socket *c = s->accept_head; c; c = c->accept_next)
            if (c->tcp && c->tcp->state != TCP_SYN_RCVD) { mask |= POLLIN; break; }
        return mask;
    }
    if (t->rcv_len || t->fin_rcvd || s->shut_rd) mask |= POLLIN;
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        if (TCP_BUF_SIZE - t->snd_len > 0 && !t->fin_queued) mask |= POLLOUT;
    }
    if (t->state == TCP_CLOSED) {
        mask |= POLLHUP | POLLIN;
        if (s->err) mask |= POLLERR;
    }
    return mask;
}

/* Free the protocol state once the socket itself is freed. */
void tcp_destroy(struct socket *s)
{
    if (s->tcp) {
        pcb_free(s->tcp);
        s->tcp = NULL;
    }
}
