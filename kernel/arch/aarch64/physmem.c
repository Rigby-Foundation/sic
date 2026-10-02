/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/physmem: 32-bit reads and writes of physical addresses below 4 GiB,
 * the file position being the address. For finding out what a board's
 * hardware looks like from a shell (ZAE `mem`). Registers whose clock is
 * off can hang the bus: this is a tool for someone who knows that. */
#include "asm/memlayout.h"
#include "fs/vfs.h"
#include "endian.h"
#include "abi/abi.h"

static long pm_read(struct file *f, void *buf, size_t len)
{
    if ((f->pos & 3) || (len & 3) || f->pos + len > 0x100000000ULL) return -EINVAL;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = mmio_read32((volatile uint8_t *)P2V(f->pos + i));
        __builtin_memcpy((uint8_t *)buf + i, &v, 4);
    }
    f->pos += len;
    return (long)len;
}

static long pm_write(struct file *f, const void *buf, size_t len)
{
    if ((f->pos & 3) || (len & 3) || f->pos + len > 0x100000000ULL) return -EINVAL;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v;
        __builtin_memcpy(&v, (const uint8_t *)buf + i, 4);
        mmio_write32((volatile uint8_t *)P2V(f->pos + i), v);
    }
    f->pos += len;
    return (long)len;
}

static const struct dev_ops pm_ops = { .read = pm_read, .write = pm_write };

void physmem_init(void)
{
    vfs_mkdev("/dev/physmem", &pm_ops, NULL);
    struct vnode *n = vfs_lookup(vfs_root(), "/dev/physmem");
    if (n) n->seekable = 1;
}
