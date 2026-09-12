/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "fs/blkdev.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

static struct blkdev *devices;

/* Partial-sector transfers go through a bounce sector. */
long blkdev_read_bytes(struct blkdev *d, uint64_t off, void *buf, size_t len)
{
    uint64_t total = d->sectors * d->sector_size;
    if (off >= total)
        return 0;
    if (off + len > total)
        len = (size_t)(total - off);
    uint8_t *out = buf;
    size_t done = 0;
    uint8_t *bounce = kmalloc(d->sector_size);
    if (!bounce)
        return -1;
    while (done < len) {
        uint64_t lba = (off + done) / d->sector_size;
        size_t   in  = (size_t)((off + done) % d->sector_size);
        size_t   n   = d->sector_size - in;
        if (n > len - done)
            n = len - done;
        if (in == 0 && n == d->sector_size) {
            if (d->read(d, lba, 1, out + done) != 0) break;
        } else {
            if (d->read(d, lba, 1, bounce) != 0) break;
            memcpy(out + done, bounce + in, n);
        }
        done += n;
    }
    kfree(bounce);
    return (long)done;
}

long blkdev_write_bytes(struct blkdev *d, uint64_t off, const void *buf, size_t len)
{
    uint64_t total = d->sectors * d->sector_size;
    if (off >= total)
        return -1;
    if (off + len > total)
        len = (size_t)(total - off);
    const uint8_t *in = buf;
    size_t done = 0;
    uint8_t *bounce = kmalloc(d->sector_size);
    if (!bounce)
        return -1;
    while (done < len) {
        uint64_t lba = (off + done) / d->sector_size;
        size_t   at  = (size_t)((off + done) % d->sector_size);
        size_t   n   = d->sector_size - at;
        if (n > len - done)
            n = len - done;
        if (at == 0 && n == d->sector_size) {
            if (d->write(d, lba, 1, in + done) != 0) break;
        } else {
            if (d->read(d, lba, 1, bounce) != 0) break;
            memcpy(bounce + at, in + done, n);
            if (d->write(d, lba, 1, bounce) != 0) break;
        }
        done += n;
    }
    kfree(bounce);
    return (long)done;
}

static long node_read(struct file *f, void *buf, size_t len)
{
    long r = blkdev_read_bytes(f->node->priv, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    return r;
}

static long node_write(struct file *f, const void *buf, size_t len)
{
    long r = blkdev_write_bytes(f->node->priv, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    return r;
}

static const struct dev_ops blk_node_ops = { node_read, node_write };

void blkdev_register(struct blkdev *d)
{
    d->next = devices;
    devices = d;
    char path[32] = "/dev/";
    memcpy(path + 5, d->name, strlen(d->name) + 1);
    vfs_mkdev(path, &blk_node_ops, d);
    struct vnode *n = vfs_lookup(vfs_root(), path);
    if (n)
        n->size = d->sectors * d->sector_size;
    kprintf("blk: %s: %lu MiB (%lu x %u)\n", d->name,
            d->sectors * d->sector_size >> 20, d->sectors, d->sector_size);
}

struct blkdev *blkdev_find(const char *name)
{
    for (struct blkdev *d = devices; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

struct blkdev *blkdev_from_vnode(struct vnode *n)
{
    if (!n || n->type != VNODE_DEV || n->dev != &blk_node_ops)
        return NULL;
    return n->priv;
}
