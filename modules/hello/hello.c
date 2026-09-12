/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* hello: the smallest useful sic kernel module. Registers /dev/hello, which
 * reads back a greeting with a counter, and unregisters it on unload. */
#include "module.h"
#include "printf.h"
#include "fs/vfs.h"
#include "string.h"
#include "arch/x86_64/timer.h"

const char module_name[] = "hello";

static int reads;

static long hello_read(struct file *f, void *buf, size_t len)
{
    char msg[96];
    if (f->pos)
        return 0;                       /* one message per open */
    reads++;
    const char *p = "hello from a kernel module! read #";
    size_t n = strlen(p);
    memcpy(msg, p, n);
    int v = reads, digits = 0;
    char tmp[12];
    do { tmp[digits++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (digits) msg[n++] = tmp[--digits];
    p = ", uptime ";
    memcpy(msg + n, p, strlen(p)); n += strlen(p);
    v = (int)(timer_ms() / 1000); digits = 0;
    do { tmp[digits++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (digits) msg[n++] = tmp[--digits];
    msg[n++] = 's'; msg[n++] = '\n';
    if (len > n) len = n;
    memcpy(buf, msg, len);
    f->pos += len;
    return (long)len;
}

static const struct dev_ops hello_ops = { hello_read, NULL };

int init_module(void)
{
    if (vfs_mkdev("/dev/hello", &hello_ops, NULL) != 0)
        return -1;
    kprintf("hello: loaded, try 'cat /dev/hello'\n");
    return 0;
}

void cleanup_module(void)
{
    vfs_unlink(vfs_root(), "/dev/hello");
    kprintf("hello: unloaded after %d read(s)\n", reads);
}
