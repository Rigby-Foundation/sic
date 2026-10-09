/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The memory devices every Unix program expects: /dev/null (swallows
 * writes, reads end at once), /dev/zero (zeros), /dev/full (writes fail:
 * no space), /dev/random and /dev/urandom (the kernel's generator). */
#include "fs/vfs.h"
#include "string.h"
#include "random.h"
#include "abi/abi.h"

static long null_read(struct file *f, void *buf, size_t len) { (void)f; (void)buf; (void)len; return 0; }
static long null_write(struct file *f, const void *buf, size_t len) { (void)f; (void)buf; return (long)len; }
static long zero_read(struct file *f, void *buf, size_t len) { (void)f; memset(buf, 0, len); return (long)len; }
static long full_write(struct file *f, const void *buf, size_t len) { (void)f; (void)buf; (void)len; return -ENOSPC; }

static long random_read(struct file *f, void *buf, size_t len)
{
    (void)f;
    uint8_t tmp[256];
    for (size_t done = 0; done < len; ) {
        size_t n = len - done < sizeof tmp ? len - done : sizeof tmp;
        random_bytes(tmp, n);
        memcpy((uint8_t *)buf + done, tmp, n);
        done += n;
    }
    return (long)len;
}

static const struct dev_ops null_ops = { .read = null_read, .write = null_write };
static const struct dev_ops zero_ops = { .read = zero_read, .write = null_write };
static const struct dev_ops full_ops = { .read = zero_read, .write = full_write };
static const struct dev_ops random_ops = { .read = random_read, .write = null_write };

void mem_devices_init(void)
{
    random_init();
    vfs_mkdev("/dev/null", &null_ops, NULL);
    vfs_mkdev("/dev/zero", &zero_ops, NULL);
    vfs_mkdev("/dev/full", &full_ops, NULL);
    vfs_mkdev("/dev/random", &random_ops, NULL);
    vfs_mkdev("/dev/urandom", &random_ops, NULL);
}
