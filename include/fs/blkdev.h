/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
struct vnode;

/* A block device: fixed sector size, synchronous transfers into kernel memory. */
struct blkdev {
    char     name[16];
    uint32_t sector_size;
    uint64_t sectors;
    int  (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int  (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
    struct blkdev *next;
};

void           blkdev_register(struct blkdev *d);     /* also creates /dev/<name> */
struct blkdev *blkdev_find(const char *name);
struct blkdev *blkdev_from_vnode(struct vnode *n);    /* NULL if not a block device node */

/* Byte-granular helpers used by the device node and filesystems. */
long blkdev_read_bytes(struct blkdev *d, uint64_t off, void *buf, size_t len);
long blkdev_write_bytes(struct blkdev *d, uint64_t off, const void *buf, size_t len);
