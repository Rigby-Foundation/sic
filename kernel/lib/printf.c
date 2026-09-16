/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "printf.h"
#include "drivers/fb.h"
#include "drivers/serial.h"
#include "types.h"
#include "string.h"
#include "spinlock.h"

static spinlock_t console_lock = SPINLOCK_INIT;

static void raw_putc(char c)
{
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

static void print_num(uint64_t v, unsigned base, int is_signed, int width, char pad, int upper)
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
        raw_putc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    uint64_t lf = spin_lock_irqsave(&console_lock);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            raw_putc(*fmt);
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
        while (*fmt == 'l') {
            longflag = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd': {
            int64_t v = longflag ? __builtin_va_arg(ap, int64_t) : __builtin_va_arg(ap, int32_t);
            print_num((uint64_t)v, 10, 1, width, pad, 0);
            break;
        }
        case 'u': {
            uint64_t v = longflag ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, uint32_t);
            print_num(v, 10, 0, width, pad, 0);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = longflag ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, uint32_t);
            print_num(v, 16, 0, width, pad, *fmt == 'X');
            break;
        }
        case 'p':
            raw_puts("0x");
            print_num((uint64_t)__builtin_va_arg(ap, void *), 16, 0, 16, '0', 0);
            break;
        case 'c':
            raw_putc((char)__builtin_va_arg(ap, int));
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            if (!left)
                for (int k = len; k < width; k++) raw_putc(' ');
            raw_puts(s);
            if (left)
                for (int k = len; k < width; k++) raw_putc(' ');
            break;
        }
        case '%':
            raw_putc('%');
            break;
        default:
            raw_putc('%');
            raw_putc(*fmt);
            break;
        }
    }

    spin_unlock_irqrestore(&console_lock, lf);
    __builtin_va_end(ap);
}
