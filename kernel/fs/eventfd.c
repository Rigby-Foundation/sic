/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* eventfd: event wait/notify through a 64-bit counter */
#include "fs/eventfd.h"
#include "proc/wait.h"
#include "mm/heap.h"
#include "string.h"
#include "abi/abi.h"

#define EVENTFD_MAX (0xfffffffffffffffeULL)

struct eventfd {
    uint64_t val;
    int flags;
    spinlock_t lock;
    struct waitqueue wq;
};

static struct eventfd *eventfd_of(struct file *f) { return f->node->priv; }

static long eventfd_read(struct file *f, void *buf, size_t len)
{
    if (len < sizeof(uint64_t))
        return -EINVAL;
    struct eventfd *ev = eventfd_of(f);
    for (;;) {
        spin_lock(&ev->lock);
        if (ev->val > 0) {
            uint64_t ret_val;
            if (ev->flags & EFD_SEMAPHORE) {
                ret_val = 1;
                ev->val -= 1;
            } else {
                ret_val = ev->val;
                ev->val = 0;
            }
            spin_unlock(&ev->lock);
            waitqueue_wake_all(&ev->wq);
            memcpy(buf, &ret_val, sizeof(ret_val));
            return (long)sizeof(uint64_t);
        }
        spin_unlock(&ev->lock);
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(&ev->wq, ev->val > 0) != 0)
            return -EINTR;
    }
}

static long eventfd_write(struct file *f, const void *buf, size_t len)
{
    if (len < sizeof(uint64_t))
        return -EINVAL;
    uint64_t add;
    memcpy(&add, buf, sizeof(add));
    if (add == 0xffffffffffffffffULL)
        return -EINVAL;
    struct eventfd *ev = eventfd_of(f);
    for (;;) {
        spin_lock(&ev->lock);
        if (EVENTFD_MAX - ev->val >= add) {
            ev->val += add;
            spin_unlock(&ev->lock);
            waitqueue_wake_all(&ev->wq);
            return (long)sizeof(uint64_t);
        }
        spin_unlock(&ev->lock);
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(&ev->wq, EVENTFD_MAX - ev->val >= add) != 0)
            return -EINTR;
    }
}

static void eventfd_release(struct file *f)
{
    struct eventfd *ev = eventfd_of(f);
    waitqueue_wake_all(&ev->wq);
    vnode_free(f->node);
    kfree(ev);
}

static int eventfd_poll(struct file *f, struct waitqueue **wq)
{
    struct eventfd *ev = eventfd_of(f);
    *wq = &ev->wq;
    int mask = 0;
    spin_lock(&ev->lock);
    if (ev->val > 0)
        mask |= POLLIN;
    if (ev->val < EVENTFD_MAX)
        mask |= POLLOUT;
    spin_unlock(&ev->lock);
    return mask;
}

static const struct dev_ops eventfd_dev_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
};

static const struct file_ops eventfd_fops = {
    .release = eventfd_release,
    .poll = eventfd_poll,
};

int eventfd_create(unsigned int initval, int flags, struct file **out)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK))
        return -EINVAL;

    struct eventfd *ev = kzalloc(sizeof(*ev));
    struct vnode *n = vnode_alloc("eventfd", 7, VNODE_DEV, NULL);
    struct file *f = kzalloc(sizeof(*f));
    if (!ev || !n || !f) {
        kfree(ev);
        kfree(n);
        kfree(f);
        return -ENOMEM;
    }

    ev->val = initval;
    ev->flags = flags;
    n->dev = &eventfd_dev_ops;
    n->priv = ev;

    f->node = n;
    f->flags = O_RDWR | ((flags & EFD_NONBLOCK) ? O_NONBLOCK : 0);
    f->refs = 1;
    f->fops = &eventfd_fops;

    *out = f;
    return 0;
}
