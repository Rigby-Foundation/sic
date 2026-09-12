/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PS/2 keyboard, scancode set 1, US layout. Echoes printable keys to the console. */
#include "drivers/keyboard.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "printf.h"
#include "proc/sched.h"
#include "spinlock.h"
#include "fs/vfs.h"

#define KBD_DATA 0x60

static const char map_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' ',
};
static const char map_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' ',
};

static int shift, caps;

/* Input ring buffer; one blocked reader at a time is plenty for a console. */
#define RING 256
static char ring[RING];
static volatile uint32_t rhead, rtail;
static struct task *reader;
static spinlock_t kbd_lock = SPINLOCK_INIT;

static void push_char(char c)
{
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    if (rhead - rtail < RING) {
        ring[rhead % RING] = c;
        rhead++;
    }
    struct task *t = reader;
    reader = NULL;
    spin_unlock_irqrestore(&kbd_lock, f);
    if (t)
        task_wake(t);
}

/* Block until at least one character is available, then drain up to len. */
long keyboard_read(char *buf, size_t len)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&kbd_lock);
        if (rhead != rtail) {
            size_t n = 0;
            while (rhead != rtail && n < len) {
                buf[n++] = ring[rtail % RING];
                rtail++;
            }
            spin_unlock_irqrestore(&kbd_lock, f);
            return (long)n;
        }
        reader = task_current();
        spin_unlock_irqrestore(&kbd_lock, f);
        task_block();
    }
}

static void keyboard_irq(struct interrupt_frame *f)
{
    (void)f;
    uint8_t sc = inb(KBD_DATA);
    int released = sc & 0x80;
    sc &= 0x7F;

    switch (sc) {
    case 0x2A: case 0x36: shift = !released; return;   /* L/R shift */
    case 0x3A: if (!released) caps = !caps; return;     /* caps lock */
    }
    if (released || sc >= 128)
        return;

    char c = map_lower[sc];
    if (!c)
        return;
    int upper = shift;
    if (caps && c >= 'a' && c <= 'z')
        upper = !upper;
    if (upper)
        c = map_upper[sc];
    kputc(c);                   /* echo */
    push_char(c);
}

static long console_read(struct file *f, void *buf, size_t len)
{
    (void)f;
    return keyboard_read(buf, len);
}

static long console_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    const char *s = buf;
    for (size_t i = 0; i < len; i++)
        kputc(s[i]);
    return (long)len;
}

const struct dev_ops console_ops = { console_read, console_write };

void keyboard_init(void)
{
    while (inb(0x64) & 1)       /* drain any pending output */
        inb(KBD_DATA);
    irq_install(1, keyboard_irq);
    irq_unmask(1);
}
