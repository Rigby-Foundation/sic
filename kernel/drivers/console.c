/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/console: writes go wherever kprintf goes, reads come from the keyboard. */
#include "drivers/console.h"
#include "printf.h"
#include "abi/abi.h"
#include "proc/syscall.h"
#ifdef CONFIG_KEYBOARD
#include "drivers/keyboard.h"
#endif

static long console_read(struct file *f, void *buf, size_t len)
{
#ifdef CONFIG_KEYBOARD
    return keyboard_read(f, buf, len, (f->flags & O_NONBLOCK) != 0);
#else
    (void)f; (void)buf; (void)len;
    return 0;                       /* no input device: EOF */
#endif
}

static long console_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    const char *s = buf;
    for (size_t i = 0; i < len; i++)
        kputc(s[i]);
    return (long)len;
}

static int console_poll(struct file *f, struct waitqueue **wq)
{
    (void)f;
#ifdef CONFIG_KEYBOARD
    return keyboard_poll(f, wq) | POLLOUT;
#else
    *wq = NULL;
    return POLLIN | POLLOUT;        /* reads return EOF at once */
#endif
}

static long console_ioctl(struct file *f, long req, uint64_t arg)
{
#ifdef CONFIG_KEYBOARD
    if (req == KDSKBMODE)
        return keyboard_set_mode(f, (int)arg);
    if (req == KDGKBMODE) {
        if (!user_ok(arg, 4)) return -EFAULT;
        *(int *)arg = keyboard_get_mode();
        return 0;
    }
#else
    (void)f; (void)arg;
#endif
    return -ENOTTY;
}

static void console_release(struct file *f)
{
#ifdef CONFIG_KEYBOARD
    keyboard_release(f);
#else
    (void)f;
#endif
}

const struct dev_ops console_ops = { .read = console_read, .write = console_write, .poll = console_poll,
                                     .ioctl = console_ioctl, .release = console_release };
