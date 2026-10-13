/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Pseudo-terminal (PTY) driver: /dev/ptmx, /dev/pts/N, and /dev/tty */
#include "drivers/pty.h"
#include "fs/vfs.h"
#include "proc/wait.h"
#include "proc/sched.h"
#include "proc/signal.h"
#include "proc/syscall.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

#define MAX_PTYS     32
#define PTY_BUF_SIZE 4096

struct pty_ring {
    uint8_t buf[PTY_BUF_SIZE];
    size_t head;
    size_t tail;
    size_t count;
};

struct pty {
    int index;
    int active;
    int locked;
    int master_open;
    int slave_open;
    int eof_pending;
    uint32_t fg_pgid;
    struct winsize ws;
    struct abi_termios termios;

    char canon_buf[512];
    int canon_len;

    struct pty_ring to_slave;   /* written by master, read by slave */
    struct pty_ring to_master;  /* written by slave / echo, read by master */

    struct waitqueue wq_master; /* master waits here for reads from to_master */
    struct waitqueue wq_slave;  /* slave waits here for reads from to_slave */

    spinlock_t lock;
};

static struct pty ptys[MAX_PTYS];
static spinlock_t pty_table_lock = SPINLOCK_INIT;

static inline size_t ring_space(const struct pty_ring *r)
{
    return PTY_BUF_SIZE - r->count;
}

static inline void ring_push(struct pty_ring *r, uint8_t ch)
{
    if (r->count < PTY_BUF_SIZE) {
        r->buf[r->head] = ch;
        r->head = (r->head + 1) % PTY_BUF_SIZE;
        r->count++;
    }
}

static inline uint8_t ring_pop(struct pty_ring *r)
{
    uint8_t ch = r->buf[r->tail];
    r->tail = (r->tail + 1) % PTY_BUF_SIZE;
    r->count--;
    return ch;
}

static inline int is_canon(const struct pty *p)
{
#ifdef __powerpc__
    return (p->termios.c_lflag & 0x00000100) != 0;
#else
    return (p->termios.c_lflag & 0x02) != 0;
#endif
}

static inline int is_echo(const struct pty *p)
{
    return (p->termios.c_lflag & 0x08) != 0;
}

static inline int is_sig(const struct pty *p)
{
#ifdef __powerpc__
    return (p->termios.c_lflag & 0x00000080) != 0;
#else
    return (p->termios.c_lflag & 0x01) != 0;
#endif
}

/* ---- Master (/dev/ptmx) ----------------------------------------------------- */

static long ptmx_open(struct file *f)
{
    spin_lock(&pty_table_lock);
    int idx = -1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spin_unlock(&pty_table_lock);
        return -ENOSPC;
    }
    struct pty *p = &ptys[idx];
    memset(p, 0, sizeof(*p));
    p->index = idx;
    p->active = 1;
    p->locked = 1;
    p->master_open = 1;
    p->slave_open = 0;
    p->ws.ws_row = 24;
    p->ws.ws_col = 80;

    p->termios.c_iflag = 0x100; /* ICRNL */
    p->termios.c_oflag = 0x5;   /* OPOST | ONLCR */
    p->termios.c_cflag = 0xbf;  /* CS8 | CREAD */
#ifdef __powerpc__
    p->termios.c_lflag = 0x00000080 | 0x00000100 | 0x00000008; /* ISIG | ICANON | ECHO */
    p->termios.c_cc[12] = 26; /* VSUSP: Ctrl-Z */
#else
    p->termios.c_lflag = 0x01 | 0x02 | 0x08; /* ISIG | ICANON | ECHO */
    p->termios.c_cc[10] = 26; /* VSUSP: Ctrl-Z */
#endif
    p->termios.c_cc[0] = 3;   /* VINTR: Ctrl-C */
    p->termios.c_cc[1] = 28;  /* VQUIT: Ctrl-\ */
    p->termios.c_cc[2] = 127; /* VERASE */
    p->termios.c_cc[3] = 21;  /* VKILL: Ctrl-U */
    p->termios.c_cc[4] = 4;   /* VEOF: Ctrl-D */

    f->priv = p;
    spin_unlock(&pty_table_lock);
    return 0;
}

static void ptmx_release(struct file *f)
{
    struct pty *p = f->priv;
    if (!p) return;

    spin_lock(&pty_table_lock);
    spin_lock(&p->lock);
    p->master_open = 0;
    uint32_t fg = p->fg_pgid;
    int destroy = (p->slave_open == 0);
    if (destroy)
        p->active = 0;
    spin_unlock(&p->lock);
    spin_unlock(&pty_table_lock);

    if (fg > 0) {
        struct task *t = task_find(fg);
        if (t) task_send_signal(t, SIGHUP);
    }
    waitqueue_wake_all(&p->wq_slave);
    waitqueue_wake_all(&p->wq_master);
}

static long ptmx_read(struct file *f, void *buf, size_t len)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    if (len == 0) return 0;

    for (;;) {
        spin_lock(&p->lock);
        if (p->to_master.count > 0) {
            size_t n = len < p->to_master.count ? len : p->to_master.count;
            for (size_t i = 0; i < n; i++)
                ((uint8_t *)buf)[i] = ring_pop(&p->to_master);
            spin_unlock(&p->lock);
            waitqueue_wake_all(&p->wq_slave);
            return (long)n;
        }
        int eof = (p->slave_open == 0);
        spin_unlock(&p->lock);
        if (eof)
            return 0;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(&p->wq_master,
                p->to_master.count > 0 || p->slave_open == 0) != 0)
            return -EINTR;
    }
}

static long ptmx_write(struct file *f, const void *buf, size_t len)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    if (len == 0) return 0;

    spin_lock(&p->lock);
    if (p->slave_open == 0) {
        spin_unlock(&p->lock);
        return -EIO;
    }
    int canon = is_canon(p);
    int echo = is_echo(p);
    int sig = is_sig(p);
    spin_unlock(&p->lock);

    if (!canon) {
        /* Raw mode */
        size_t done = 0;
        while (done < len) {
            spin_lock(&p->lock);
            if (p->slave_open == 0) {
                spin_unlock(&p->lock);
                return done ? (long)done : -EIO;
            }
            size_t sp = ring_space(&p->to_slave);
            if (sp > 0) {
                size_t n = len - done < sp ? len - done : sp;
                for (size_t i = 0; i < n; i++) {
                    uint8_t c = ((const uint8_t *)buf)[done + i];
                    if (echo)
                        ring_push(&p->to_master, c);
                    ring_push(&p->to_slave, c);
                }
                done += n;
                spin_unlock(&p->lock);
                waitqueue_wake_all(&p->wq_slave);
                if (echo)
                    waitqueue_wake_all(&p->wq_master);
                continue;
            }
            spin_unlock(&p->lock);
            if (f->flags & O_NONBLOCK)
                return done ? (long)done : -EAGAIN;
            if (wait_event_interruptible(&p->wq_master,
                    ring_space(&p->to_slave) > 0 || p->slave_open == 0) != 0)
                return done ? (long)done : -EINTR;
        }
        return (long)done;
    }

    /* Canonical (cooked) mode */
    spin_lock(&p->lock);
    if (p->slave_open == 0) {
        spin_unlock(&p->lock);
        return -EIO;
    }

    const uint8_t *src = buf;
    int echoed = 0;
    int flushed = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (sig) {
            if (c == 3) { /* Ctrl-C */
                if (echo) {
                    ring_push(&p->to_master, '^');
                    ring_push(&p->to_master, 'C');
                    ring_push(&p->to_master, '\n');
                    echoed = 1;
                }
                p->canon_len = 0;
                uint32_t fg = p->fg_pgid;
                if (fg > 0) {
                    struct task *t = task_find(fg);
                    if (t) task_send_signal(t, SIGINT);
                }
                continue;
            }
            if (c == 26) { /* Ctrl-Z */
                if (echo) {
                    ring_push(&p->to_master, '^');
                    ring_push(&p->to_master, 'Z');
                    ring_push(&p->to_master, '\n');
                    echoed = 1;
                }
                p->canon_len = 0;
                uint32_t fg = p->fg_pgid;
                if (fg > 0) {
                    struct task *t = task_find(fg);
                    if (t) task_send_signal(t, SIGTSTP);
                }
                continue;
            }
            if (c == 28) { /* Ctrl-\ */
                if (echo) {
                    ring_push(&p->to_master, '^');
                    ring_push(&p->to_master, '\\');
                    ring_push(&p->to_master, '\n');
                    echoed = 1;
                }
                p->canon_len = 0;
                uint32_t fg = p->fg_pgid;
                if (fg > 0) {
                    struct task *t = task_find(fg);
                    if (t) task_send_signal(t, SIGQUIT);
                }
                continue;
            }
        }

        if (c == '\r' && (p->termios.c_iflag & 0x100))
            c = '\n';

        if (c == '\b' || c == 127) {
            if (p->canon_len > 0) {
                p->canon_len--;
                if (echo) {
                    ring_push(&p->to_master, '\b');
                    ring_push(&p->to_master, ' ');
                    ring_push(&p->to_master, '\b');
                    echoed = 1;
                }
            }
            continue;
        }

        if (c == 4) { /* Ctrl-D */
            if (p->canon_len > 0) {
                for (int k = 0; k < p->canon_len; k++)
                    ring_push(&p->to_slave, (uint8_t)p->canon_buf[k]);
                p->canon_len = 0;
                flushed = 1;
            } else {
                p->eof_pending = 1;
                flushed = 1;
            }
            continue;
        }

        if (echo) {
            if (c == '\n') {
                ring_push(&p->to_master, '\r');
                ring_push(&p->to_master, '\n');
            } else {
                ring_push(&p->to_master, c);
            }
            echoed = 1;
        }

        if (p->canon_len < (int)sizeof(p->canon_buf) - 1)
            p->canon_buf[p->canon_len++] = (char)c;

        if (c == '\n' || p->canon_len >= (int)sizeof(p->canon_buf) - 1) {
            for (int k = 0; k < p->canon_len; k++)
                ring_push(&p->to_slave, (uint8_t)p->canon_buf[k]);
            p->canon_len = 0;
            flushed = 1;
        }
    }
    spin_unlock(&p->lock);

    if (echoed)
        waitqueue_wake_all(&p->wq_master);
    if (flushed)
        waitqueue_wake_all(&p->wq_slave);

    return (long)len;
}

static int ptmx_poll(struct file *f, struct waitqueue **wq)
{
    struct pty *p = f->priv;
    if (!p) { *wq = NULL; return POLLERR; }
    *wq = &p->wq_master;
    int mask = 0;
    spin_lock(&p->lock);
    if (p->to_master.count > 0) mask |= POLLIN;
    if (p->slave_open == 0) mask |= POLLHUP;
    if (p->slave_open > 0 && ring_space(&p->to_slave) > 0) mask |= POLLOUT;
    spin_unlock(&p->lock);
    return mask;
}

static long ptmx_ioctl(struct file *f, long req, uint64_t arg)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    uint32_t r = (uint32_t)req;

    if (r == TIOCGPTN) {
        if (!user_ok(arg, sizeof(unsigned int))) return -EFAULT;
        unsigned int idx = (unsigned int)p->index;
        memcpy((void *)arg, &idx, sizeof(idx));
        return 0;
    }
    if (r == TIOCSPTLCK) {
        if (!user_ok(arg, sizeof(int))) return -EFAULT;
        int lock;
        memcpy(&lock, (const void *)arg, sizeof(lock));
        spin_lock(&p->lock);
        p->locked = lock ? 1 : 0;
        spin_unlock(&p->lock);
        return 0;
    }
    if (r == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        spin_lock(&p->lock);
        struct winsize ws = p->ws;
        spin_unlock(&p->lock);
        memcpy((void *)arg, &ws, sizeof(ws));
        return 0;
    }
    if (r == TIOCSWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize ws;
        memcpy(&ws, (const void *)arg, sizeof(ws));
        spin_lock(&p->lock);
        p->ws = ws;
        uint32_t fg = p->fg_pgid;
        spin_unlock(&p->lock);
        if (fg > 0) {
            struct task *t = task_find(fg);
            if (t) task_send_signal(t, SIGWINCH);
        }
        return 0;
    }
    return -ENOTTY;
}

static const struct dev_ops ptmx_ops = {
    .tty = 0,
    .open = ptmx_open,
    .release = ptmx_release,
    .read = ptmx_read,
    .write = ptmx_write,
    .poll = ptmx_poll,
    .ioctl = ptmx_ioctl,
};

/* ---- Slave (/dev/pts/N) ----------------------------------------------------- */

static long pts_open(struct file *f)
{
    int idx = (int)(uintptr_t)f->node->priv;
    if (idx < 0 || idx >= MAX_PTYS)
        return -ENODEV;

    spin_lock(&pty_table_lock);
    struct pty *p = &ptys[idx];
    if (!p->active || p->locked) {
        spin_unlock(&pty_table_lock);
        return -EIO;
    }
    p->slave_open++;
    f->priv = p;
    spin_unlock(&pty_table_lock);
    return 0;
}

static void pts_release(struct file *f)
{
    struct pty *p = f->priv;
    if (!p) return;

    spin_lock(&pty_table_lock);
    spin_lock(&p->lock);
    if (p->slave_open > 0)
        p->slave_open--;
    int destroy = (p->master_open == 0 && p->slave_open == 0);
    if (destroy)
        p->active = 0;
    spin_unlock(&p->lock);
    spin_unlock(&pty_table_lock);

    waitqueue_wake_all(&p->wq_master);
    waitqueue_wake_all(&p->wq_slave);
}

static long pts_read(struct file *f, void *buf, size_t len)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    if (len == 0) return 0;

    for (;;) {
        spin_lock(&p->lock);
        if (p->to_slave.count > 0) {
            size_t n = len < p->to_slave.count ? len : p->to_slave.count;
            for (size_t i = 0; i < n; i++)
                ((uint8_t *)buf)[i] = ring_pop(&p->to_slave);
            spin_unlock(&p->lock);
            waitqueue_wake_all(&p->wq_master);
            return (long)n;
        }
        if (p->eof_pending) {
            p->eof_pending = 0;
            spin_unlock(&p->lock);
            return 0;
        }
        int eof = (p->master_open == 0);
        spin_unlock(&p->lock);
        if (eof)
            return 0;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(&p->wq_slave,
                p->to_slave.count > 0 || p->master_open == 0 || p->eof_pending) != 0)
            return -EINTR;
    }
}

static long pts_write(struct file *f, const void *buf, size_t len)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    if (len == 0) return 0;

    size_t done = 0;
    while (done < len) {
        spin_lock(&p->lock);
        if (p->master_open == 0) {
            spin_unlock(&p->lock);
            return done ? (long)done : -EIO;
        }
        size_t sp = ring_space(&p->to_master);
        if (sp > 0) {
            size_t n = len - done < sp ? len - done : sp;
            for (size_t i = 0; i < n; i++) {
                uint8_t c = ((const uint8_t *)buf)[done + i];
                if (c == '\n' && (p->termios.c_oflag & 0x04)) { /* ONLCR */
                    if (ring_space(&p->to_master) >= 2) {
                        ring_push(&p->to_master, '\r');
                        ring_push(&p->to_master, '\n');
                    } else {
                        ring_push(&p->to_master, '\n');
                    }
                } else {
                    ring_push(&p->to_master, c);
                }
            }
            done += n;
            spin_unlock(&p->lock);
            waitqueue_wake_all(&p->wq_master);
            continue;
        }
        spin_unlock(&p->lock);
        if (f->flags & O_NONBLOCK)
            return done ? (long)done : -EAGAIN;
        if (wait_event_interruptible(&p->wq_slave,
                ring_space(&p->to_master) > 0 || p->master_open == 0) != 0)
            return done ? (long)done : -EINTR;
    }
    return (long)done;
}

static int pts_poll(struct file *f, struct waitqueue **wq)
{
    struct pty *p = f->priv;
    if (!p) { *wq = NULL; return POLLERR; }
    *wq = &p->wq_slave;
    int mask = 0;
    spin_lock(&p->lock);
    if (p->to_slave.count > 0 || p->eof_pending) mask |= POLLIN;
    if (p->master_open == 0) mask |= POLLHUP;
    if (p->master_open > 0 && ring_space(&p->to_master) > 0) mask |= POLLOUT;
    spin_unlock(&p->lock);
    return mask;
}

static long pts_ioctl(struct file *f, long req, uint64_t arg)
{
    struct pty *p = f->priv;
    if (!p) return -EBADF;
    uint32_t r = (uint32_t)req;

    if (r == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        spin_lock(&p->lock);
        struct winsize ws = p->ws;
        spin_unlock(&p->lock);
        memcpy((void *)arg, &ws, sizeof(ws));
        return 0;
    }
    if (r == TIOCSWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize ws;
        memcpy(&ws, (const void *)arg, sizeof(ws));
        spin_lock(&p->lock);
        p->ws = ws;
        uint32_t fg = p->fg_pgid;
        spin_unlock(&p->lock);
        if (fg > 0) {
            struct task *t = task_find(fg);
            if (t) task_send_signal(t, SIGWINCH);
        }
        return 0;
    }
    if (r == TCGETS) {
        if (!user_ok(arg, sizeof(struct abi_termios))) return -EFAULT;
        spin_lock(&p->lock);
        struct abi_termios tio = p->termios;
        spin_unlock(&p->lock);
        memcpy((void *)arg, &tio, sizeof(tio));
        return 0;
    }
    if (r == TCSETS || r == TCSETSW || r == TCSETSF) {
        if (!user_ok(arg, sizeof(struct abi_termios))) return -EFAULT;
        struct abi_termios tio;
        memcpy(&tio, (const void *)arg, sizeof(tio));
        spin_lock(&p->lock);
        p->termios = tio;
        spin_unlock(&p->lock);
        return 0;
    }
    if (r == TIOCGPGRP) {
        if (!user_ok(arg, sizeof(uint32_t))) return -EFAULT;
        spin_lock(&p->lock);
        uint32_t fg = p->fg_pgid;
        spin_unlock(&p->lock);
        memcpy((void *)arg, &fg, sizeof(fg));
        return 0;
    }
    if (r == TIOCSPGRP) {
        if (!user_ok(arg, sizeof(uint32_t))) return -EFAULT;
        uint32_t fg;
        memcpy(&fg, (const void *)arg, sizeof(fg));
        spin_lock(&p->lock);
        p->fg_pgid = fg;
        spin_unlock(&p->lock);
        return 0;
    }
    return -ENOTTY;
}

static const struct dev_ops pts_ops = {
    .tty = 1,
    .open = pts_open,
    .release = pts_release,
    .read = pts_read,
    .write = pts_write,
    .poll = pts_poll,
    .ioctl = pts_ioctl,
};

/* ---- Controlling terminal (/dev/tty) ---------------------------------------- */

static struct file *current_tty_file(void)
{
    struct task *t = task_current();
    if (t && t->fdt) {
        for (int fd = 0; fd < 3; fd++) {
            struct file *f = fd_get(fd);
            if (f && f->node && f->node->type == VNODE_DEV && f->node->dev && f->node->dev->tty)
                return f;
        }
    }
    return NULL;
}

static long tty_dev_read(struct file *f, void *buf, size_t len)
{
    (void)f;
    struct file *target = current_tty_file();
    if (!target) return -ENXIO;
    return file_read(target, buf, len);
}

static long tty_dev_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    struct file *target = current_tty_file();
    if (!target) return -ENXIO;
    return file_write(target, buf, len);
}

static int tty_dev_poll(struct file *f, struct waitqueue **wq)
{
    (void)f;
    struct file *target = current_tty_file();
    if (!target) { *wq = NULL; return POLLERR; }
    return file_poll(target, wq);
}

static long tty_dev_ioctl(struct file *f, long req, uint64_t arg)
{
    (void)f;
    struct file *target = current_tty_file();
    if (!target || !target->node->dev) return -ENXIO;
    if (target->node->dev->ioctl)
        return target->node->dev->ioctl(target, req, arg);
    return -ENOTTY;
}

static const struct dev_ops tty_dev_ops = {
    .tty = 1,
    .read = tty_dev_read,
    .write = tty_dev_write,
    .poll = tty_dev_poll,
    .ioctl = tty_dev_ioctl,
};

/* ---- Registration ----------------------------------------------------------- */

void pty_init(void)
{
    if (!vfs_lookup(vfs_root(), "/dev/pts"))
        vfs_create(vfs_root(), "/dev/pts", VNODE_DIR);

    vfs_mkdev("/dev/ptmx", &ptmx_ops, NULL);
    vfs_mkdev("/dev/tty", &tty_dev_ops, NULL);

    for (int i = 0; i < MAX_PTYS; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), "/dev/pts/%d", i);
        vfs_mkdev(name, &pts_ops, (void *)(uintptr_t)i);
    }
}
