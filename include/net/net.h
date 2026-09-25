/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The network stack (CONFIG_NET): packets, interfaces, Ethernet/ARP/IPv4,
 * ICMP, UDP, TCP and BSD sockets on top.
 *
 * Every IP address and port in these structures is in network byte order,
 * exactly as it appears on the wire and in struct sockaddr_in.
 *
 * Locking: one lock, net_lock, protects the stack's state (sockets, TCP
 * control blocks, the ARP cache, interface addresses). Drivers never take it:
 * their interrupt handlers only note that work is pending and wake the
 * `netrx` kernel thread, which calls netdev.poll() with the lock held and
 * runs the protocol timers. Nothing sleeps under net_lock. */
#pragma once
#include "types.h"
#include "spinlock.h"
#include "proc/wait.h"
#include "abi/abi.h"
#include "endian.h"

extern spinlock_t net_lock;

static inline uint16_t htons(uint16_t v) { return htobe16(v); }
static inline uint16_t ntohs(uint16_t v) { return be16toh(v); }
static inline uint32_t htonl(uint32_t v) { return htobe32(v); }
static inline uint32_t ntohl(uint32_t v) { return be32toh(v); }
#define IP4(a, b, c, d) htonl(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---- packets ------------------------------------------------------------------ */
/* One buffer per frame; headers are pushed in front of the payload, so the
 * payload starts with `headroom` free bytes before it. */
#define PKT_SIZE     2048
#define PKT_HEADROOM 128
#define PKT_DATA_MAX (PKT_SIZE - 64 - PKT_HEADROOM)

struct pkt {
    struct pkt *next;
    uint32_t off, len;              /* payload = buf + off, len bytes */
    uint32_t src_ip, dst_ip;        /* receive path: the IP header's addresses */
    uint16_t src_port, proto;
    uint16_t ihl;                   /* receive path: IP header length */
    struct netdev *dev;             /* receive path: arrival interface */
    uint8_t  buf[PKT_SIZE - 64];
};

struct pkt *pkt_alloc(void);                        /* off = PKT_HEADROOM, len = 0 */
void        pkt_free(struct pkt *p);
static inline uint8_t *pkt_data(struct pkt *p) { return p->buf + p->off; }
static inline void    *pkt_push(struct pkt *p, uint32_t n) { p->off -= n; p->len += n; return p->buf + p->off; }
static inline void    *pkt_pull(struct pkt *p, uint32_t n) { p->off += n; p->len -= n; return p->buf + p->off; }
static inline uint32_t pkt_tailroom(struct pkt *p) { return (uint32_t)sizeof(p->buf) - p->off - p->len; }

/* ---- interfaces --------------------------------------------------------------- */
struct netdev {
    char     name[IFNAMSIZ];
    int      index;
    uint8_t  mac[6];
    uint16_t mtu;
    int      flags;                 /* IFF_* */
    uint32_t ip, mask, gw;          /* 0 = unset */
    int (*xmit)(struct netdev *d, struct pkt *p);   /* takes ownership of p */
    void (*poll)(struct netdev *d);                 /* drain received frames: net_rx() each */
    volatile int rx_pending;        /* set by the driver's IRQ handler */
    void    *priv;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
    struct netdev *next;
};

int            netdev_register(struct netdev *d);
struct netdev *netdev_first(void);
struct netdev *netdev_by_name(const char *name);
struct netdev *netdev_by_index(int index);
void           netdev_kick(struct netdev *d);       /* IRQ context: rx_pending = 1, wake netrx */
static inline uint32_t netdev_bcast(struct netdev *d) { return d->ip | ~d->mask; }

/* ---- Ethernet / ARP ----------------------------------------------------------- */
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806
extern const uint8_t eth_broadcast[6];

void net_rx(struct netdev *d, struct pkt *p);       /* a received frame (net_lock held) */
int  eth_send(struct netdev *d, const uint8_t *dst, uint16_t proto, struct pkt *p);
int  arp_output(struct netdev *d, uint32_t nexthop, struct pkt *p);  /* resolve, queue if needed */
void arp_rx(struct netdev *d, struct pkt *p);
void arp_timer(uint64_t now_ms);

/* ---- IPv4 --------------------------------------------------------------------- */
struct ip_hdr {
    uint8_t  ver_ihl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

struct netdev *ip_route(uint32_t dst, uint32_t *nexthop);
int  ip_local(uint32_t addr);                       /* one of our addresses (or loopback) */
int  ip_send(uint32_t src, uint32_t dst, uint8_t proto, struct pkt *p);   /* src 0 = pick */
void ip_rx(struct netdev *d, struct pkt *p);
uint16_t ip_checksum(const void *data, size_t len);
uint16_t ip_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len);

/* ---- ICMP / UDP / TCP --------------------------------------------------------- */
void icmp_rx(struct pkt *p);
void udp_rx(struct pkt *p);
void tcp_rx(struct pkt *p);
void tcp_timer(uint64_t now_ms);

/* ---- sockets ------------------------------------------------------------------ */
enum tcp_state { TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
                 TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT };

#define TCP_BUF_SIZE (32 * 1024)
#define TCP_MSS      1460
#define TCP_MAX_SACK 8

struct tcp_pcb {
    enum tcp_state state;
    uint32_t iss, irs;
    uint32_t snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint32_t rcv_nxt;
    uint16_t mss;
    uint8_t *sndbuf;                /* bytes [snd_una, snd_una + snd_len) */
    uint32_t snd_len;
    uint8_t *rcvbuf;                /* ring: rcv_head, rcv_len */
    uint32_t rcv_head, rcv_len;
    int      fin_queued, fin_sent, fin_rcvd;
    uint64_t rto_at, rto_ms;        /* retransmit timer, 0 = off */
    uint64_t retry_at;              /* a send failed (ring full, no route yet): try again then, 0 = off */
    int      retries;
    uint64_t timewait_at;
    int      dup_acks;
    /* SACK (RFC 2018): received out-of-order data waits in `ooo` until the
     * hole before it fills; what the peer has SACKed is skipped on retransmit */
    int      sack_ok;               /* negotiated on the SYN */
    struct tcp_ooo *ooo;            /* sorted by seq, non-overlapping */
    uint32_t ooo_bytes;
    struct { uint32_t start, end; } sacked[TCP_MAX_SACK];
    int      nsacked;
    uint32_t recover;               /* fast retransmit: only holes below this are resent */
    /* delayed ACK (RFC 1122): one ACK per two full segments or 40 ms */
    int      ack_pending, ack_segs;
    uint32_t rcv_wnd_adv;           /* the window the last segment we sent advertised */
    uint64_t delack_at;
};

struct tcp_ooo {
    struct tcp_ooo *next;
    uint32_t seq, len;
    uint8_t *data;
};

struct unix_pcb;
struct socket {
    int domain;                     /* AF_INET or AF_UNIX */
    int type, proto;                /* SOCK_STREAM/DGRAM/RAW, IPPROTO_* */
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    int  bound, connected, listening, shut_rd, shut_wr;
    int  err;                       /* pending asynchronous error (SO_ERROR), as +errno */
    int  reuseaddr, broadcast;
    long rcvtimeo_ms, sndtimeo_ms;  /* 0 = forever */
    struct pkt *rxq_head, *rxq_tail;    /* datagram queue (UDP, raw) */
    uint32_t rxq_bytes;
    struct waitqueue wq;            /* everything about this socket */
    int  refs, listed;              /* listed: on its protocol's socket list */
    struct tcp_pcb *tcp;
    struct unix_pcb *un;            /* AF_UNIX (unix.c) */
    struct socket *accept_head, *accept_tail, *accept_next;  /* listener: connections ready */
    int  backlog, accept_count;
    struct socket *listener;        /* for connections still in the backlog */
    struct socket *next;            /* all sockets of a protocol */
};

struct socket *sock_create(int type, int proto);
void  sock_get(struct socket *s);
void  sock_put(struct socket *s);   /* free when the last user and the protocol are done */
void  sock_deliver(struct socket *s, struct pkt *p);   /* queue a datagram (net_lock held) */
void  sock_link(struct socket *s);  /* put it on its protocol's list (once) */

/* TCP entry points used by the socket layer (net_lock held) */
int   tcp_connect(struct socket *s);
int   tcp_listen(struct socket *s);
int   tcp_send(struct socket *s, const void *buf, size_t len);      /* bytes queued or -errno */
int   tcp_recv(struct socket *s, void *buf, size_t len, int peek);  /* bytes, 0 = EOF, -EAGAIN */
void  tcp_shutdown_wr(struct socket *s);
void  tcp_close(struct socket *s);
int   tcp_poll(struct socket *s);
struct socket *tcp_lookup(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport);
uint16_t tcp_ephemeral_port(void);
uint16_t udp_ephemeral_port(void);
int   udp_send(struct socket *s, uint32_t dst_ip, uint16_t dst_port, struct pkt *p);   /* takes p */
void  tcp_destroy(struct socket *s);
extern struct socket *udp_sockets, *tcp_sockets, *raw_sockets;
struct file *sock_file(struct socket *s, int flags);   /* a file over a socket (SOCK_NONBLOCK honoured) */

/* AF_UNIX stream sockets (unix.c); the ones marked so run with net_lock held */
long  unix_socket(int type, int flags);                 /* an fd or -errno */
int   unix_bind(struct socket *s, const char *path);
int   unix_listen(struct socket *s, int backlog);
int   unix_connect(struct socket *s, const char *path);
long  unix_accept(struct socket *l, struct file *f, int flags);
long  unix_send(struct file *f, const void *buf, size_t len, int flags);
long  unix_recv(struct file *f, void *buf, size_t len, int flags);
int   unix_poll(struct socket *s);                      /* locked */
void  unix_shutdown(struct socket *s, int how);         /* locked */
void  unix_close(struct socket *s);                     /* locked: disconnect, unbind, free the pcb */
const char *unix_name(struct socket *s);

/* ---- ioctl / init ------------------------------------------------------------- */
long net_ioctl(long req, void *arg);
void net_init(void);                /* loopback + the netrx thread; drivers register later */
