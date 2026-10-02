/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "printf.h"
#include "drivers/fb.h"
#include "drivers/serial.h"
#include "types.h"
#include "string.h"
#include "spinlock.h"

static spinlock_t console_lock = SPINLOCK_INIT;

/* Everything printed, the last KLOG_SIZE bytes of it: /proc/kmsg. */
#define KLOG_SIZE 65536
static char klog[KLOG_SIZE];
static uint64_t klog_total;

size_t klog_read(uint64_t off, char *dst, size_t n)
{
    uint64_t f = spin_lock_irqsave(&console_lock);
    uint64_t start = klog_total > KLOG_SIZE ? klog_total - KLOG_SIZE : 0;
    size_t got = 0;
    for (uint64_t p = start + off; p < klog_total && got < n; p++) dst[got++] = klog[p % KLOG_SIZE];
    spin_unlock_irqrestore(&console_lock, f);
    return got;
}

static void raw_putc(char c)
{
    klog[klog_total++ % KLOG_SIZE] = c;
#ifdef CONFIG_FB_CONSOLE
    fb_putc(c);
#endif
#ifdef CONFIG_SERIAL
    serial_putc(c);
#endif
#if !defined(CONFIG_FB_CONSOLE) && !defined(CONFIG_SERIAL)
    (void)c;
#endif
}

void kputc(char c)
{
    uint64_t f = spin_lock_irqsave(&console_lock);
    raw_putc(c);
    spin_unlock_irqrestore(&console_lock, f);
}

static void raw_puts(const char *s)
{
    while (*s)
        raw_putc(*s++);
}

void kputs(const char *s)
{
    uint64_t f = spin_lock_irqsave(&console_lock);
    raw_puts(s);
    spin_unlock_irqrestore(&console_lock, f);
}

/* Formatting, into the console or a buffer. */
struct out {
    void (*put)(struct out *o, char c);
    char *buf; size_t cap, len;         /* a buffer: what fits, and how much was asked for */
};

static void out_console(struct out *o, char c) { (void)o; raw_putc(c); }
static void out_buf(struct out *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}
static void out_str(struct out *o, const char *s) { while (*s) o->put(o, *s++); }

static void print_num(struct out *out, uint64_t v, unsigned base, int is_signed, int width, char pad, int upper)
{
    char buf[32];
    int i = 0, neg = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (is_signed && (int64_t)v < 0) {
        neg = 1;
        v = (uint64_t)(-(int64_t)v);
    }
    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        buf[i++] = '-';
    while (i < width)
        buf[i++] = pad;
    while (i--)
        out->put(out, buf[i]);
}

static void format(struct out *out, const char *fmt, __builtin_va_list *ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out->put(out, *fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0, longflag = 0, left = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        /* l = long (native word), ll = 64-bit, z = size_t (a long) */
        while (*fmt == 'l' || *fmt == 'z') {
            longflag++;
            fmt++;
        }
        if (longflag == 1 && sizeof(long) == 8)
            longflag = 2;

        switch (*fmt) {
        case 'd': {
            int64_t v = longflag >= 2 ? __builtin_va_arg(*ap, int64_t) : longflag ? __builtin_va_arg(*ap, long) : __builtin_va_arg(*ap, int32_t);
            print_num(out, (uint64_t)v, 10, 1, width, pad, 0);
            break;
        }
        case 'u': {
            uint64_t v = longflag >= 2 ? __builtin_va_arg(*ap, uint64_t) : longflag ? __builtin_va_arg(*ap, unsigned long) : __builtin_va_arg(*ap, uint32_t);
            print_num(out, v, 10, 0, width, pad, 0);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = longflag >= 2 ? __builtin_va_arg(*ap, uint64_t) : longflag ? __builtin_va_arg(*ap, unsigned long) : __builtin_va_arg(*ap, uint32_t);
            print_num(out, v, 16, 0, width, pad, *fmt == 'X');
            break;
        }
        case 'p':
            out_str(out, "0x");
            print_num(out, (uintptr_t)__builtin_va_arg(*ap, void *), 16, 0, 2 * (int)sizeof(void *), '0', 0);
            break;
        case 'c':
            out->put(out, (char)__builtin_va_arg(*ap, int));
            break;
        case 's': {
            const char *s = __builtin_va_arg(*ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            if (!left)
                for (int k = len; k < width; k++) out->put(out, ' ');
            out_str(out, s);
            if (left)
                for (int k = len; k < width; k++) out->put(out, ' ');
            break;
        }
        case '%':
            out->put(out, '%');
            break;
        default:
            out->put(out, '%');
            out->put(out, *fmt);
            break;
        }
    }

}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    struct out o = { out_console, NULL, 0, 0 };
    uint64_t lf = spin_lock_irqsave(&console_lock);
    format(&o, fmt, &ap);
    spin_unlock_irqrestore(&console_lock, lf);
    __builtin_va_end(ap);
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, __builtin_va_list ap)
{
    struct out o = { out_buf, buf, cap, 0 };
    __builtin_va_list copy;
    __builtin_va_copy(copy, ap);
    format(&o, fmt, &copy);
    __builtin_va_end(copy);
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}
