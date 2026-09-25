/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* AF_UNIX stream sockets: a pair of ring buffers between two processes on
 * this machine, no packets, no checksums, no windows. Names are kept in a
 * kernel table rather than as filesystem nodes: bind() claims the path,
 * closing the socket gives it back, so there are no stale socket files.
 * net_lock and the socket's waitqueue serve here as for the IP sockets. */
#include "net/net.h"
#include "net/socket.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "proc/signal.h"
#include "proc/syscall.h"
#include "string.h"

#define UNIX_BUF_SIZE (256 * 1024)          /* per direction */
#define UNIX_PATH_MAX 108

struct unix_pcb {
    char path[UNIX_PATH_MAX];               /* bound name, "" if none */
    struct socket *peer;                    /* the other end, NULL once it closed */
    uint8_t *rx;                            /* what the peer sent us: a ring */
    uint32_t head, tail;                    /* read at head, write at tail, in bytes */
    int eof;                                /* the peer will not send more */
    struct socket *sock;
    struct socket *next_bound;
};

static struct socket *bound;                /* named sockets (net_lock) */

static uint32_t rx_count(struct unix_pcb *u) { return u->tail - u->head; }
static uint32_t rx_space(struct unix_pcb *u) { return UNIX_BUF_SIZE - rx_count(u); }

static struct unix_pcb *pcb_new(struct socket *s)
{
    struct unix_pcb *u = kzalloc(sizeof(*u));
    if (!u) return NULL;
    u->sock = s;
    s->un = u;
    return u;
}

static int pcb_rx_alloc(struct unix_pcb *u)
{
    if (u->rx) return 0;
    u->rx = kmalloc(UNIX_BUF_SIZE);
    return u->rx ? 0 : -ENOMEM;
}

long unix_socket(int type, int flags)
{
    if ((type & SOCK_TYPE_MASK) != SOCK_STREAM) return -EPROTONOSUPPORT;
    struct socket *s = sock_create(SOCK_STREAM, 0);
    if (!s) return -ENOMEM;
    s->domain = AF_UNIX;
    if (!pcb_new(s)) { kfree(s); return -ENOMEM; }
    struct file *f = sock_file(s, flags);
    if (!f) { kfree(s->un); kfree(s); return -ENOMEM; }
    int fd = fd_install(f);
    if (fd < 0) file_close(f);
    return fd;
}

/* net_lock held */
static struct socket *find_bound(const char *path)
{
    for (struct socket *s = bound; s; s = s->un->next_bound)
        if (strcmp(s->un->path, path) == 0) return s;
    return NULL;
}

int unix_bind(struct socket *s, const char *path)
{
    if (!path[0]) return -EINVAL;
    spin_lock(&net_lock);
    int r = 0;
    if (s->bound) r = -EINVAL;
    else if (find_bound(path)) r = -EADDRINUSE;
    else {
        strcpy(s->un->path, path);
        s->un->next_bound = bound;
        bound = s;
        s->bound = 1;
    }
    spin_unlock(&net_lock);
    return r;
}

int unix_listen(struct socket *s, int backlog)
{
    spin_lock(&net_lock);
    int r = s->bound ? 0 : -EINVAL;
    if (!r) {
        s->listening = 1;
        s->backlog = backlog < 1 ? 1 : backlog > 64 ? 64 : backlog;
    }
    spin_unlock(&net_lock);
    return r;
}

int unix_connect(struct socket *s, const char *path)
{
    spin_lock(&net_lock);
    int r = 0;
    struct socket *l = find_bound(path);
    if (s->un->peer || s->connected) r = -EISCONN;
    else if (!l) r = -ENOENT;
    else if (!l->listening) r = -ECONNREFUSED;
    else if (l->accept_count >= l->backlog) r = -EAGAIN;
    else {
        /* the server side of the connection, queued for accept() */
        struct socket *c = sock_create(SOCK_STREAM, 0);
        if (!c || !pcb_new(c) || pcb_rx_alloc(c->un) || pcb_rx_alloc(s->un)) {
            if (c) { kfree(c->un); kfree(c); }
            r = -ENOMEM;
        } else {
            c->domain = AF_UNIX;
            c->connected = 1;
            c->un->peer = s;
            s->un->peer = c;
            s->connected = 1;
            c->listener = l;
            c->accept_next = NULL;
            if (l->accept_tail) l->accept_tail->accept_next = c; else l->accept_head = c;
            l->accept_tail = c;
            l->accept_count++;
            waitqueue_wake_all(&l->wq);
        }
    }
    spin_unlock(&net_lock);
    return r;
}

static int accept_ready(struct socket *l)
{
    spin_lock(&net_lock);
    int r = l->accept_head != NULL;
    spin_unlock(&net_lock);
    return r;
}

long unix_accept(struct socket *l, struct file *f, int flags)
{
    struct socket *c;
    for (;;) {
        spin_lock(&net_lock);
        c = l->accept_head;
        if (c) {
            l->accept_head = c->accept_next;
            if (!l->accept_head) l->accept_tail = NULL;
            l->accept_count--;
            c->listener = NULL;
        }
        spin_unlock(&net_lock);
        if (c) break;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(&l->wq, accept_ready(l))) return -EINTR;
    }
    struct file *cf = sock_file(c, flags);
    if (!cf) {
        spin_lock(&net_lock);
        sock_put(c);
        spin_unlock(&net_lock);
        return -ENOMEM;
    }
    int fd = fd_install(cf);
    if (fd < 0) file_close(cf);
    return fd;
}

/* net_lock held */
int unix_poll(struct socket *s)
{
    struct unix_pcb *u = s->un;
    int m = 0;
    if (s->listening) return s->accept_head ? POLLIN : 0;
    if (rx_count(u) || u->eof || s->shut_rd) m |= POLLIN;
    if (!u->peer && s->connected) m |= POLLHUP;
    else if (u->peer && rx_space(u->peer->un) > 0 && !s->shut_wr) m |= POLLOUT;
    return m;
}

static int poll_has(struct socket *s, int want)
{
    spin_lock(&net_lock);
    int m = unix_poll(s);
    spin_unlock(&net_lock);
    return m & want;
}

static int wait_for(struct socket *s, int want, long timeo_ms)
{
    if (timeo_ms > 0) {
        int r = wait_event_timeout(&s->wq, poll_has(s, want), timeo_ms);
        return r > 0 ? 0 : r == 0 ? -EAGAIN : -EINTR;
    }
    return wait_event_interruptible(&s->wq, poll_has(s, want)) ? -EINTR : 0;
}

static int nonblocking(struct file *f, int flags) { return (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT); }

long unix_send(struct file *f, const void *buf, size_t len, int flags)
{
    struct socket *s = f->node->priv;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        spin_lock(&net_lock);
        struct socket *peer = s->un->peer;
        if (!peer || s->shut_wr) {
            spin_unlock(&net_lock);
            if (done) return (long)done;
            if (!(flags & MSG_NOSIGNAL)) task_send_signal(task_current(), 13);
            return -EPIPE;
        }
        struct unix_pcb *d = peer->un;
        uint32_t space = rx_space(d), n = len - done < space ? (uint32_t)(len - done) : space;
        if (n) {
            uint32_t off = d->tail % UNIX_BUF_SIZE, first = UNIX_BUF_SIZE - off < n ? UNIX_BUF_SIZE - off : n;
            memcpy(d->rx + off, p + done, first);
            if (n > first) memcpy(d->rx, p + done + first, n - first);
            d->tail += n;
            done += n;
            waitqueue_wake_all(&peer->wq);
        }
        spin_unlock(&net_lock);
        if (done == len) break;
        if (nonblocking(f, flags)) return done ? (long)done : -EAGAIN;
        int w = wait_for(s, POLLOUT | POLLHUP, s->sndtimeo_ms);
        if (w) return done ? (long)done : w;
    }
    return (long)done;
}

long unix_recv(struct file *f, void *buf, size_t len, int flags)
{
    struct socket *s = f->node->priv;
    struct unix_pcb *u = s->un;
    for (;;) {
        spin_lock(&net_lock);
        uint32_t avail = rx_count(u), n = len < avail ? (uint32_t)len : avail;
        long r;
        if (n) {
            uint32_t off = u->head % UNIX_BUF_SIZE, first = UNIX_BUF_SIZE - off < n ? UNIX_BUF_SIZE - off : n;
            memcpy(buf, u->rx + off, first);
            if (n > first) memcpy((uint8_t *)buf + first, u->rx, n - first);
            if (!(flags & MSG_PEEK)) {
                u->head += n;
                if (u->peer) waitqueue_wake_all(&u->peer->wq);      /* room to send */
            }
            r = n;
        } else if (u->eof || s->shut_rd || !s->connected) {
            r = s->connected ? 0 : -ENOTCONN;
        } else {
            r = -EAGAIN;
        }
        spin_unlock(&net_lock);
        if (r != -EAGAIN) return r;
        if (nonblocking(f, flags)) return -EAGAIN;
        int w = wait_for(s, POLLIN | POLLHUP, s->rcvtimeo_ms);
        if (w) return w;
    }
}

/* net_lock held */
void unix_shutdown(struct socket *s, int how)
{
    if ((how == SHUT_WR || how == SHUT_RDWR) && s->un->peer) {
        s->un->peer->un->eof = 1;
        waitqueue_wake_all(&s->un->peer->wq);
    }
}

/* The last file on the socket went away. net_lock held. */
void unix_close(struct socket *s)
{
    struct unix_pcb *u = s->un;
    if (!u) return;
    if (s->bound)
        for (struct socket **pp = &bound; *pp; pp = &(*pp)->un->next_bound)
            if (*pp == s) { *pp = u->next_bound; break; }
    /* connections nobody accepted: their clients see a hangup */
    while (s->accept_head) {
        struct socket *c = s->accept_head;
        s->accept_head = c->accept_next;
        sock_put(c);
    }
    if (u->peer) {
        u->peer->un->peer = NULL;
        u->peer->un->eof = 1;
        waitqueue_wake_all(&u->peer->wq);
    }
    kfree(u->rx);
    kfree(u);
    s->un = NULL;
}

const char *unix_name(struct socket *s) { return s->un ? s->un->path : ""; }
