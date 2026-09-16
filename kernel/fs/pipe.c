/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Anonymous pipes: a ring buffer with one wait queue per direction. */
#include "fs/pipe.h"
#include "proc/wait.h"
#include "mm/heap.h"
#include "string.h"
#include "abi/abi.h"

#define PIPE_SIZE (64 * 1024)

struct pipe {
    uint8_t *buf;
    size_t head, tail;              /* head = write index, tail = read index (mod PIPE_SIZE) */
    size_t count;
    int readers, writers;
    spinlock_t lock;
    struct waitqueue readable, writable;
};

static struct pipe *pipe_of(struct file *f) { return f->node->priv; }

static long pipe_read(struct file *f, void *buf, size_t len)
{
    struct pipe *p = pipe_of(f);
    if (len == 0)
        return 0;
    for (;;) {
        spin_lock(&p->lock);
        if (p->count > 0) {
            size_t n = len < p->count ? len : p->count;
            for (size_t i = 0; i < n; i++) {
                ((uint8_t *)buf)[i] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
            }
            p->count -= n;
            spin_unlock(&p->lock);
            waitqueue_wake_all(&p->writable);
            return (long)n;
        }
        int eof = p->writers == 0;
        spin_unlock(&p->lock);
        if (eof)
            return 0;
        if (wait_event_interruptible(&p->readable, p->count > 0 || p->writers == 0) != 0)
            return -EINTR;
    }
}

static long pipe_write(struct file *f, const void *buf, size_t len)
{
    struct pipe *p = pipe_of(f);
    size_t done = 0;
    while (done < len) {
        spin_lock(&p->lock);
        if (p->readers == 0) {
            spin_unlock(&p->lock);
            task_send_signal(task_current(), 13 /* SIGPIPE */);
            return done ? (long)done : -EPIPE;
        }
        size_t space = PIPE_SIZE - p->count;
        if (space > 0) {
            size_t n = len - done < space ? len - done : space;
            for (size_t i = 0; i < n; i++) {
                p->buf[p->head] = ((const uint8_t *)buf)[done + i];
                p->head = (p->head + 1) % PIPE_SIZE;
            }
            p->count += n;
            done += n;
            spin_unlock(&p->lock);
            waitqueue_wake_all(&p->readable);
            continue;
        }
        spin_unlock(&p->lock);
        if (wait_event_interruptible(&p->writable, p->count < PIPE_SIZE || p->readers == 0) != 0)
            return done ? (long)done : -EINTR;
    }
    return (long)done;
}

static void pipe_release(struct file *f)
{
    struct pipe *p = pipe_of(f);
    spin_lock(&p->lock);
    if ((f->flags & 3) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    int gone = p->readers == 0 && p->writers == 0;
    spin_unlock(&p->lock);
    waitqueue_wake_all(&p->readable);
    waitqueue_wake_all(&p->writable);
    if (gone) {
        kfree(p->buf);
        kfree(p);
        vnode_free(f->node);
    }
}

static int pipe_poll(struct file *f, struct waitqueue **wq)
{
    struct pipe *p = pipe_of(f);
    int mask = 0;
    spin_lock(&p->lock);
    if ((f->flags & 3) == O_RDONLY) {
        *wq = &p->readable;
        if (p->count > 0) mask |= POLLIN;
        if (p->writers == 0) mask |= POLLHUP;
    } else {
        *wq = &p->writable;
        if (p->count < PIPE_SIZE) mask |= POLLOUT;
        if (p->readers == 0) mask |= POLLERR;
    }
    spin_unlock(&p->lock);
    return mask;
}

static const struct dev_ops pipe_ops = { .read = pipe_read, .write = pipe_write };
static const struct file_ops pipe_fops = { .release = pipe_release, .poll = pipe_poll };

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));
    struct vnode *n = vnode_alloc("pipe", 4, VNODE_DEV, NULL);
    struct file *r = kzalloc(sizeof(*r)), *w = kzalloc(sizeof(*w));
    if (!p || !n || !r || !w || !(p->buf = kmalloc(PIPE_SIZE))) {
        if (p) kfree(p->buf);
        kfree(p); kfree(n); kfree(r); kfree(w);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;
    n->dev = &pipe_ops;
    n->priv = p;
    r->node = w->node = n;
    r->flags = O_RDONLY;
    w->flags = O_WRONLY;
    r->refs = w->refs = 1;
    r->fops = w->fops = &pipe_fops;
    *rd = r;
    *wr = w;
    return 0;
}
