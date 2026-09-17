/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* BSD sockets: the socket objects, their file integration, the socket
 * system calls and the interface-configuration ioctls. */
#include "net/net.h"
#include "net/socket.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "proc/signal.h"
#include "proc/syscall.h"
#include "string.h"
#include "printf.h"

struct socket *udp_sockets, *tcp_sockets, *raw_sockets;

static struct socket **list_for(struct socket *s)
{
    return s->type == SOCK_STREAM ? &tcp_sockets : s->type == SOCK_DGRAM ? &udp_sockets : &raw_sockets;
}

struct socket *sock_create(int type, int proto)
{
    struct socket *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->type = type;
    s->proto = proto;
    s->refs = 1;
    s->wq = (struct waitqueue)WAITQUEUE_INIT;
    return s;
}

void sock_link(struct socket *s)
{
    if (s->listed)
        return;
    struct socket **l = list_for(s);
    s->next = *l;
    *l = s;
    s->listed = 1;
}

void sock_get(struct socket *s) { s->refs++; }

/* net_lock held */
void sock_put(struct socket *s)
{
    if (--s->refs > 0)
        return;
    if (s->listed)
        for (struct socket **pp = list_for(s); *pp; pp = &(*pp)->next)
            if (*pp == s) { *pp = s->next; break; }
    while (s->rxq_head) {
        struct pkt *p = s->rxq_head;
        s->rxq_head = p->next;
        pkt_free(p);
    }
    tcp_destroy(s);
    kfree(s);
}

#define RXQ_MAX (64 * 1024)

void sock_deliver(struct socket *s, struct pkt *p)
{
    if (s->rxq_bytes + p->len > RXQ_MAX || s->shut_rd) {
        pkt_free(p);
        return;
    }
    p->next = NULL;
    if (s->rxq_tail) s->rxq_tail->next = p; else s->rxq_head = p;
    s->rxq_tail = p;
    s->rxq_bytes += p->len;
    waitqueue_wake_all(&s->wq);
}

/* ---- the file side ------------------------------------------------------------- */
static struct socket *sock_of(struct file *f) { return f->node->priv; }
static int nonblocking(struct file *f, int flags) { return (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT); }

static int dgram_poll(struct socket *s)
{
    int mask = POLLOUT;
    if (s->rxq_head || s->shut_rd) mask |= POLLIN;
    if (s->err) mask |= POLLERR;
    return mask;
}

static int sock_poll_locked(struct socket *s)
{
    return s->type == SOCK_STREAM ? tcp_poll(s) : dgram_poll(s);
}

static int sock_poll(struct file *f, struct waitqueue **wq)
{
    struct socket *s = sock_of(f);
    *wq = &s->wq;
    spin_lock(&net_lock);
    int m = sock_poll_locked(s);
    spin_unlock(&net_lock);
    return m;
}

static void sock_release(struct file *f)
{
    struct socket *s = sock_of(f);
    spin_lock(&net_lock);
    if (s->type == SOCK_STREAM)
        tcp_close(s);
    sock_put(s);
    spin_unlock(&net_lock);
    vnode_free(f->node);
}

static long do_recvfrom(struct file *f, void *buf, size_t len, int flags, struct abi_sockaddr_in *from);
static long do_sendto(struct file *f, const void *buf, size_t len, int flags, const struct abi_sockaddr_in *to);

static long sock_read(struct file *f, void *buf, size_t len)   { return do_recvfrom(f, buf, len, 0, NULL); }
static long sock_write(struct file *f, const void *buf, size_t len) { return do_sendto(f, buf, len, 0, NULL); }

static const struct dev_ops sock_dev_ops = { .read = sock_read, .write = sock_write };
static const struct file_ops sock_fops = { .release = sock_release, .poll = sock_poll };

static struct file *sock_file(struct socket *s, int flags)
{
    struct vnode *n = vnode_alloc("socket", 6, VNODE_DEV, NULL);
    struct file *f = kzalloc(sizeof(*f));
    if (!n || !f) {
        kfree(n);
        kfree(f);
        return NULL;
    }
    n->dev = &sock_dev_ops;
    n->priv = s;
    n->is_sock = 1;
    f->node = n;
    f->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    f->refs = 1;
    f->fops = &sock_fops;
    return f;
}

static int sock_ready(struct socket *s, int want)
{
    spin_lock(&net_lock);
    int m = sock_poll_locked(s);
    spin_unlock(&net_lock);
    return m & want;
}

/* Wait for the socket's poll mask to include one of `want`. Returns 0, or
 * -EINTR / -EAGAIN (timeout, when timeo_ms > 0). */
static int sock_wait(struct socket *s, int want, long timeo_ms)
{
    if (timeo_ms > 0) {
        int r = wait_event_timeout(&s->wq, sock_ready(s, want), timeo_ms);
        return r > 0 ? 0 : r == 0 ? -EAGAIN : -EINTR;
    }
    return wait_event_interruptible(&s->wq, sock_ready(s, want)) ? -EINTR : 0;
}

/* ---- addresses ------------------------------------------------------------------ */
static int addr_in(uint64_t uaddr, uint64_t ulen, struct abi_sockaddr_in *out)
{
    if (ulen < sizeof(*out) || !user_ok(uaddr, sizeof(*out)))
        return -EINVAL;
    memcpy(out, (const void *)uaddr, sizeof(*out));
    if (out->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    return 0;
}

static int addr_out(uint64_t uaddr, uint64_t ulenp, uint32_t ip, uint16_t port)
{
    if (!uaddr)
        return 0;
    if (!user_ok(ulenp, 4)) return -EFAULT;
    uint32_t len = *(uint32_t *)ulenp;
    struct abi_sockaddr_in a = { .sin_family = AF_INET, .sin_port = port, .sin_addr = ip };
    uint32_t n = len < sizeof(a) ? len : sizeof(a);
    if (!user_ok(uaddr, n)) return -EFAULT;
    memcpy((void *)uaddr, &a, n);
    *(uint32_t *)ulenp = sizeof(a);
    return 0;
}

/* Is (ip, port) free for a socket of s's protocol? net_lock held. */
static int port_free(struct socket *s, uint32_t ip, uint16_t port)
{
    for (struct socket *o = *list_for(s); o; o = o->next) {
        if (o == s || o->local_port != port)
            continue;
        if (o->local_ip && ip && o->local_ip != ip)
            continue;
        if (o->reuseaddr && s->reuseaddr)
            continue;
        if (o->tcp && o->tcp->state == TCP_TIME_WAIT)
            continue;
        return 0;
    }
    return 1;
}

static int bind_locked(struct socket *s, uint32_t ip, uint16_t port)
{
    if (s->bound)
        return -EINVAL;
    if (ip && !ip_local(ip))
        return -EADDRNOTAVAIL;
    if (port == 0) {
        port = s->type == SOCK_STREAM ? tcp_ephemeral_port() : udp_ephemeral_port();
        if (!port)
            return -EADDRINUSE;
    } else if (!port_free(s, ip, port)) {
        return -EADDRINUSE;
    }
    s->local_ip = ip;
    s->local_port = port;
    s->bound = 1;
    sock_link(s);
    return 0;
}

/* ---- system calls --------------------------------------------------------------- */
long sys_socket(long domain, long type, long proto)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    int flags = (int)type & ~SOCK_TYPE_MASK;
    type &= SOCK_TYPE_MASK;
    switch (type) {
    case SOCK_STREAM: if (proto == 0) proto = IPPROTO_TCP; if (proto != IPPROTO_TCP) return -EPROTONOSUPPORT; break;
    case SOCK_DGRAM:  if (proto == 0) proto = IPPROTO_UDP; if (proto != IPPROTO_UDP) return -EPROTONOSUPPORT; break;
    case SOCK_RAW:    if (proto != IPPROTO_ICMP) return -EPROTONOSUPPORT; break;
    default: return -EINVAL;
    }
    struct socket *s = sock_create((int)type, (int)proto);
    if (!s)
        return -ENOMEM;
    struct file *f = sock_file(s, flags);
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }
    if (type == SOCK_RAW) {
        spin_lock(&net_lock);
        sock_link(s);
        spin_unlock(&net_lock);
    }
    int fd = fd_install(f);
    if (fd < 0)
        file_close(f);
    return fd;
}

static struct socket *sock_fd(long fd, struct file **fp)
{
    struct file *f = fd_get(fd);
    if (!f || !f->node->is_sock)
        return NULL;
    *fp = f;
    return f->node->priv;
}

long sys_bind(long fd, uint64_t uaddr, uint64_t ulen)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    struct abi_sockaddr_in a;
    int r = addr_in(uaddr, ulen, &a);
    if (r) return r;
    spin_lock(&net_lock);
    r = bind_locked(s, a.sin_addr, a.sin_port);
    spin_unlock(&net_lock);
    return r;
}

long sys_listen(long fd, long backlog)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    spin_lock(&net_lock);
    int r = 0;
    if (!s->bound)
        r = bind_locked(s, 0, 0);
    if (!r && s->listening)
        r = 0;
    else if (!r)
        r = tcp_listen(s);
    if (!r)
        s->backlog = backlog < 1 ? 1 : backlog > 64 ? 64 : (int)backlog;
    spin_unlock(&net_lock);
    return r;
}

long sys_connect(long fd, uint64_t uaddr, uint64_t ulen)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    struct abi_sockaddr_in a;
    int r = addr_in(uaddr, ulen, &a);
    if (r) return r;

    spin_lock(&net_lock);
    if (s->type != SOCK_STREAM) {           /* datagram: just remember the peer */
        if (!s->bound && (r = bind_locked(s, 0, 0)) != 0) goto out;
        s->remote_ip = a.sin_addr;
        s->remote_port = a.sin_port;
        s->connected = a.sin_addr != 0;
        goto out;
    }
    if (s->tcp) {
        if (s->tcp->state == TCP_SYN_SENT) r = -EALREADY;
        else if (s->tcp->state == TCP_ESTABLISHED) r = -EISCONN;
        else r = s->err ? -s->err : -ECONNREFUSED;
        if (r == -ECONNREFUSED) s->err = 0;
        goto out;
    }
    if (!a.sin_addr || !a.sin_port) { r = -ECONNREFUSED; goto out; }
    uint32_t nexthop;
    struct netdev *d = ip_route(a.sin_addr, &nexthop);
    if (!d || !d->ip) { r = -ENETUNREACH; goto out; }
    if (!s->bound && (r = bind_locked(s, 0, 0)) != 0) goto out;
    if (!s->local_ip)
        s->local_ip = d->ip;
    s->remote_ip = a.sin_addr;
    s->remote_port = a.sin_port;
    r = tcp_connect(s);
    if (r) goto out;
    if (f->flags & O_NONBLOCK) { r = -EINPROGRESS; goto out; }
    spin_unlock(&net_lock);
    /* block until the handshake ends one way or the other */
    int w = wait_event_interruptible(&s->wq, s->tcp->state != TCP_SYN_SENT && s->tcp->state != TCP_SYN_RCVD);
    spin_lock(&net_lock);
    if (w) r = -EINTR;
    else if (s->tcp->state == TCP_CLOSED) { r = s->err ? -s->err : -ECONNREFUSED; s->err = 0; }
    else r = 0;
out:
    spin_unlock(&net_lock);
    return r;
}

long sys_accept4(long fd, uint64_t uaddr, uint64_t ulenp, long flags)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (s->type != SOCK_STREAM || !s->listening) return -EINVAL;
    struct socket *c = NULL;
    for (;;) {
        spin_lock(&net_lock);
        for (struct socket *x = s->accept_head; x; x = x->accept_next)
            if (x->tcp && x->tcp->state != TCP_SYN_RCVD) { c = x; break; }
        if (c) {
            struct socket **pp = &s->accept_head;
            while (*pp != c) pp = &(*pp)->accept_next;
            *pp = c->accept_next;
            if (s->accept_tail == c) {
                s->accept_tail = NULL;
                for (struct socket *x = s->accept_head; x; x = x->accept_next) s->accept_tail = x;
            }
            s->accept_count--;
            c->listener = NULL;
            sock_get(c);                        /* the new file's reference */
        }
        spin_unlock(&net_lock);
        if (c) break;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(&s->wq, sock_ready(s, POLLIN)))
            return -EINTR;
    }
    struct file *cf = sock_file(c, (int)flags);
    if (!cf) {
        spin_lock(&net_lock);
        tcp_close(c);
        sock_put(c);
        spin_unlock(&net_lock);
        return -ENOMEM;
    }
    int r = addr_out(uaddr, ulenp, c->remote_ip, c->remote_port);
    int nfd = r ? r : fd_install(cf);
    if (nfd < 0) file_close(cf);
    return nfd;
}

long sys_shutdown(long fd, long how)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (how < 0 || how > 2) return -EINVAL;
    spin_lock(&net_lock);
    if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = 1;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        s->shut_wr = 1;
        if (s->type == SOCK_STREAM) tcp_shutdown_wr(s);
    }
    waitqueue_wake_all(&s->wq);
    spin_unlock(&net_lock);
    return 0;
}

long sys_getsockname(long fd, uint64_t uaddr, uint64_t ulenp)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    return addr_out(uaddr, ulenp, s->local_ip, s->local_port);
}

long sys_getpeername(long fd, uint64_t uaddr, uint64_t ulenp)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!s->connected) return -ENOTCONN;
    return addr_out(uaddr, ulenp, s->remote_ip, s->remote_port);
}

long sys_setsockopt(long fd, long level, long name, uint64_t uval, uint64_t len)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (len < 4 || !user_ok(uval, len)) return -EINVAL;
    int v = *(const int *)uval;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR: s->reuseaddr = v != 0; return 0;
        case SO_BROADCAST: s->broadcast = v != 0; return 0;
        case SO_KEEPALIVE: case SO_SNDBUF: case SO_RCVBUF: return 0;
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            if (len < sizeof(struct abi_ktimeval)) return -EINVAL;
            const struct abi_ktimeval *tv = (const void *)uval;
            long ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
            if (name == SO_RCVTIMEO) s->rcvtimeo_ms = ms; else s->sndtimeo_ms = ms;
            return 0;
        }
        }
        return -ENOPROTOOPT;
    }
    if (level == SOL_TCP && name == TCP_NODELAY) return 0;     /* there is no Nagle to disable */
    if (level == IPPROTO_IP) return 0;
    return -ENOPROTOOPT;
}

long sys_getsockopt(long fd, long level, long name, uint64_t uval, uint64_t ulenp)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!user_ok(ulenp, 4)) return -EFAULT;
    uint32_t len = *(uint32_t *)ulenp;
    if (len < 4 || !user_ok(uval, 4)) return -EINVAL;
    int v;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_ERROR:     spin_lock(&net_lock); v = s->err; s->err = 0; spin_unlock(&net_lock); break;
        case SO_REUSEADDR: v = s->reuseaddr; break;
        case SO_BROADCAST: v = s->broadcast; break;
        case SO_KEEPALIVE: v = 0; break;
        case SO_SNDBUF: case SO_RCVBUF: v = TCP_BUF_SIZE; break;
        case 3 /* SO_TYPE */: v = s->type; break;
        default: return -ENOPROTOOPT;
        }
    } else if (level == SOL_TCP && name == TCP_NODELAY) {
        v = 1;
    } else {
        return -ENOPROTOOPT;
    }
    *(int *)uval = v;
    *(uint32_t *)ulenp = 4;
    return 0;
}

/* ---- data transfer -------------------------------------------------------------- */
static long do_sendto(struct file *f, const void *buf, size_t len, int flags, const struct abi_sockaddr_in *to)
{
    struct socket *s = sock_of(f);
    if (s->type == SOCK_STREAM) {
        if (to) return -EISCONN;
        size_t done = 0;
        while (done < len) {
            spin_lock(&net_lock);
            int r = tcp_send(s, (const uint8_t *)buf + done, len - done);
            spin_unlock(&net_lock);
            if (r == -EAGAIN) {
                if (done && nonblocking(f, flags)) break;
                if (nonblocking(f, flags)) return -EAGAIN;
                int w = sock_wait(s, POLLOUT | POLLHUP | POLLERR, s->sndtimeo_ms);
                if (w == -EINTR) return done ? (long)done : -EINTR;
                if (w == -EAGAIN) return done ? (long)done : -EAGAIN;
                continue;
            }
            if (r < 0) {
                if (r == -EPIPE && !(flags & MSG_NOSIGNAL))
                    task_send_signal(task_current(), 13);
                return done ? (long)done : r;
            }
            done += (size_t)r;
        }
        return (long)done;
    }

    /* datagrams */
    uint32_t dip; uint16_t dport;
    if (to) { dip = to->sin_addr; dport = to->sin_port; }
    else if (s->connected) { dip = s->remote_ip; dport = s->remote_port; }
    else return -EDESTADDRREQ;
    if (len > PKT_DATA_MAX) return -EMSGSIZE;
    struct pkt *p = pkt_alloc();
    if (!p) return -ENOMEM;
    memcpy(pkt_data(p), buf, len);
    p->len = (uint32_t)len;
    spin_lock(&net_lock);
    int r;
    if (s->type == SOCK_DGRAM) {
        if (!s->bound && (r = bind_locked(s, 0, 0)) != 0) { pkt_free(p); goto out; }
        if ((dip == 0xFFFFFFFF || (ntohl(dip) & 0xFF) == 0xFF) && !s->broadcast) { pkt_free(p); r = -EACCES; goto out; }
        r = udp_send(s, dip, dport, p);
    } else {
        r = ip_send(s->local_ip, dip, IPPROTO_ICMP, p);
    }
out:
    spin_unlock(&net_lock);
    return r < 0 ? r : (long)len;
}

static long do_recvfrom(struct file *f, void *buf, size_t len, int flags, struct abi_sockaddr_in *from)
{
    struct socket *s = sock_of(f);
    if (s->type == SOCK_STREAM) {
        for (;;) {
            spin_lock(&net_lock);
            int r = tcp_recv(s, buf, len, flags & MSG_PEEK);
            spin_unlock(&net_lock);
            if (r != -EAGAIN) {
                if (from && r >= 0) { from->sin_family = AF_INET; from->sin_addr = s->remote_ip; from->sin_port = s->remote_port; }
                return r;
            }
            if (nonblocking(f, flags)) return -EAGAIN;
            int w = sock_wait(s, POLLIN | POLLHUP | POLLERR, s->rcvtimeo_ms);
            if (w) return w;
        }
    }
    for (;;) {
        spin_lock(&net_lock);
        struct pkt *p = s->rxq_head;
        long r;
        if (p) {
            uint32_t n = len < p->len ? (uint32_t)len : p->len;
            memcpy(buf, pkt_data(p), n);
            if (from) { from->sin_family = AF_INET; from->sin_addr = p->src_ip; from->sin_port = p->src_port; }
            if (!(flags & MSG_PEEK)) {
                s->rxq_head = p->next;
                if (!s->rxq_head) s->rxq_tail = NULL;
                s->rxq_bytes -= p->len;
                pkt_free(p);
            }
            r = n;
        } else if (s->shut_rd) {
            r = 0;
        } else {
            r = -EAGAIN;
        }
        spin_unlock(&net_lock);
        if (r != -EAGAIN) return r;
        if (nonblocking(f, flags)) return -EAGAIN;
        int w = sock_wait(s, POLLIN | POLLERR, s->rcvtimeo_ms);
        if (w) return w;
    }
}

long sys_sendto(long fd, uint64_t ubuf, uint64_t len, long flags, uint64_t uaddr, uint64_t ulen)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!user_ok(ubuf, len)) return -EFAULT;
    struct abi_sockaddr_in a;
    if (uaddr) {
        int r = addr_in(uaddr, ulen, &a);
        if (r) return r;
    }
    return do_sendto(f, (const void *)ubuf, len, (int)flags, uaddr ? &a : NULL);
}

long sys_recvfrom(long fd, uint64_t ubuf, uint64_t len, long flags, uint64_t uaddr, uint64_t ulenp)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!user_ok(ubuf, len)) return -EFAULT;
    struct abi_sockaddr_in a = { 0 };
    long r = do_recvfrom(f, (void *)ubuf, len, (int)flags, uaddr ? &a : NULL);
    if (r >= 0 && uaddr) {
        int e = addr_out(uaddr, ulenp, a.sin_addr, a.sin_port);
        if (e) return e;
    }
    return r;
}

/* sendmsg/recvmsg: gather/scatter through one bounce buffer. */
#define MSG_MAX (64 * 1024)

static long iov_total(const struct abi_iovec *iov, int cnt)
{
    uint64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!user_ok(iov[i].iov_base, iov[i].iov_len)) return -EFAULT;
        total += iov[i].iov_len;
    }
    return total > MSG_MAX ? MSG_MAX : (long)total;
}

long sys_sendmsg(long fd, uint64_t umsg, long flags)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!user_ok(umsg, sizeof(struct abi_msghdr))) return -EFAULT;
    const struct abi_msghdr *m = (const void *)umsg;
    if (m->msg_iovlen < 0 || m->msg_iovlen > 64 || !user_ok(m->msg_iov, (uint64_t)m->msg_iovlen * sizeof(struct abi_iovec)))
        return -EINVAL;
    const struct abi_iovec *iov = (const void *)m->msg_iov;
    long total = iov_total(iov, m->msg_iovlen);
    if (total < 0) return total;
    struct abi_sockaddr_in a;
    if (m->msg_name) {
        int r = addr_in(m->msg_name, m->msg_namelen, &a);
        if (r) return r;
    }
    uint8_t *buf = kmalloc(total ? (size_t)total : 1);
    if (!buf) return -ENOMEM;
    size_t off = 0;
    for (int i = 0; i < m->msg_iovlen && off < (size_t)total; i++) {
        size_t n = iov[i].iov_len < (size_t)total - off ? iov[i].iov_len : (size_t)total - off;
        memcpy(buf + off, (const void *)iov[i].iov_base, n);
        off += n;
    }
    long r = do_sendto(f, buf, (size_t)total, (int)flags, m->msg_name ? &a : NULL);
    kfree(buf);
    return r;
}

long sys_recvmsg(long fd, uint64_t umsg, long flags)
{
    struct file *f;
    struct socket *s = sock_fd(fd, &f);
    if (!s) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (!user_ok(umsg, sizeof(struct abi_msghdr))) return -EFAULT;
    struct abi_msghdr *m = (void *)umsg;
    if (m->msg_iovlen < 0 || m->msg_iovlen > 64 || !user_ok(m->msg_iov, (uint64_t)m->msg_iovlen * sizeof(struct abi_iovec)))
        return -EINVAL;
    const struct abi_iovec *iov = (const void *)m->msg_iov;
    long total = iov_total(iov, m->msg_iovlen);
    if (total < 0) return total;
    uint8_t *buf = kmalloc(total ? (size_t)total : 1);
    if (!buf) return -ENOMEM;
    struct abi_sockaddr_in a = { 0 };
    long r = do_recvfrom(f, buf, (size_t)total, (int)flags, m->msg_name ? &a : NULL);
    if (r > 0) {
        size_t off = 0;
        for (int i = 0; i < m->msg_iovlen && off < (size_t)r; i++) {
            size_t n = iov[i].iov_len < (size_t)r - off ? iov[i].iov_len : (size_t)r - off;
            memcpy((void *)iov[i].iov_base, buf + off, n);
            off += n;
        }
    }
    kfree(buf);
    if (r >= 0 && m->msg_name) {
        uint32_t n = m->msg_namelen < sizeof(a) ? m->msg_namelen : sizeof(a);
        if (!user_ok(m->msg_name, n)) return -EFAULT;
        memcpy((void *)m->msg_name, &a, n);
        m->msg_namelen = sizeof(a);
    }
    if (r >= 0) {
        m->msg_controllen = 0;
        m->msg_flags = 0;
    }
    return r;
}

/* ---- interface configuration ---------------------------------------------------- */
long net_ioctl(long req, void *arg)
{
    struct abi_ifreq *ifr = arg;
    long r = 0;
    spin_lock(&net_lock);
    struct netdev *d;
    if (req == SIOCGIFNAME) {
        d = netdev_by_index(ifr->ifr_ifindex);
        if (d) strcpy(ifr->ifr_name, d->name); else r = -ENODEV;
        goto out;
    }
    ifr->ifr_name[IFNAMSIZ - 1] = 0;
    d = netdev_by_name(ifr->ifr_name);
    if (!d) { r = -ENODEV; goto out; }
    struct abi_sockaddr_in *a = &ifr->ifr_addr;
    switch (req) {
    case SIOCGIFINDEX:   ifr->ifr_ifindex = d->index; break;
    case SIOCGIFFLAGS:   ifr->ifr_flags = (int16_t)d->flags; break;
    case SIOCSIFFLAGS:
        d->flags = (d->flags & ~IFF_UP) | (ifr->ifr_flags & IFF_UP);
        break;
    case SIOCGIFMTU:     ifr->ifr_mtu = d->mtu; break;
    case SIOCGIFHWADDR:
        memset(ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        ifr->ifr_hwaddr[0] = 1;             /* ARPHRD_ETHER */
        memcpy(ifr->ifr_hwaddr + 2, d->mac, 6);
        break;
    case SIOCGIFADDR:    memset(a, 0, sizeof(*a)); a->sin_family = AF_INET; a->sin_addr = d->ip; break;
    case SIOCGIFNETMASK: memset(a, 0, sizeof(*a)); a->sin_family = AF_INET; a->sin_addr = d->mask; break;
    case SIOCGIFGATEWAY: memset(a, 0, sizeof(*a)); a->sin_family = AF_INET; a->sin_addr = d->gw; break;
    case SIOCGIFBRDADDR: memset(a, 0, sizeof(*a)); a->sin_family = AF_INET; a->sin_addr = d->ip ? netdev_bcast(d) : 0; break;
    case SIOCSIFADDR:
        if (a->sin_family != AF_INET) { r = -EAFNOSUPPORT; break; }
        d->ip = a->sin_addr;
        if (d->ip && !d->mask) d->mask = IP4(255, 255, 255, 0);
        if (d->ip) d->flags |= IFF_UP | IFF_RUNNING;
        break;
    case SIOCSIFNETMASK:
        if (a->sin_family != AF_INET) { r = -EAFNOSUPPORT; break; }
        d->mask = a->sin_addr;
        break;
    case SIOCSIFGATEWAY:
        if (a->sin_family != AF_INET) { r = -EAFNOSUPPORT; break; }
        d->gw = a->sin_addr;
        break;
    default:
        r = -ENOTTY;
    }
out:
    spin_unlock(&net_lock);
    return r;
}
