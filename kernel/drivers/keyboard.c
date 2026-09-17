/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Console input: the PS/2 keyboard (x86, scancode set 1, US layout) and the
 * serial port, merged into one ring for /dev/console. Printable keys are
 * echoed. */
#include "drivers/keyboard.h"
#include "asm/irq.h"
#ifdef CONFIG_SERIAL
#include "drivers/serial.h"
#endif
#ifdef __x86_64__
#include "asm/io.h"
#endif
#include "printf.h"
#include "proc/sched.h"
#include "spinlock.h"
#include "proc/wait.h"
#include "proc/signal.h"
#include "abi/abi.h"
#include "fs/vfs.h"

#ifdef __x86_64__
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

static int shift, caps, ctrl;
#endif

/* Input ring buffer; one blocked reader at a time is plenty for a console. */
#define RING 256
static char ring[RING];
static volatile uint32_t rhead, rtail;
static spinlock_t kbd_lock = SPINLOCK_INIT;
static struct waitqueue readers = WAITQUEUE_INIT;
static volatile uint32_t fg_pid;            /* foreground process for ^C (TIOCSPGRP) */

static void push_char(char c)
{
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    if (rhead - rtail < RING) {
        ring[rhead % RING] = c;
        rhead++;
    }
    spin_unlock_irqrestore(&kbd_lock, f);
    waitqueue_wake_all(&readers);
}

#include "drivers/fb.h"

void keyboard_push_char(char c)
{
    if (c == 3) {                                       /* ^C from serial */
        kputs("^C\n");
        struct task *t = fg_pid ? task_find(fg_pid) : NULL;
        if (t)
            task_send_signal(t, SIGINT);
        return;
    }
    if (!fb_is_graphics_mode())
        kputc(c);                                       /* echo */
    push_char(c);
}

/* Drains the serial port. Called from the receive interrupt and from readers
 * that are about to block; the two must not interleave on the port (the UART
 * holds one byte, and status-then-data from two contexts reads it twice). */
static void poll_serial_input(void)
{
#ifdef CONFIG_SERIAL
    static spinlock_t serial_in_lock = SPINLOCK_INIT;
    uint64_t f = spin_lock_irqsave(&serial_in_lock);
    int c;
    while ((c = serial_getc()) >= 0) {
        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';
        keyboard_push_char((char)c);
    }
    spin_unlock_irqrestore(&serial_in_lock, f);
#endif
}

/* Drain up to len characters. If nonblock is set and no input is available,
 * returns -EAGAIN. Otherwise blocks until input arrives. Returns -EINTR on signal. */
long keyboard_read(char *buf, size_t len, int nonblock)
{
    for (;;) {
        poll_serial_input();
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
        spin_unlock_irqrestore(&kbd_lock, f);
        if (nonblock)
            return -EAGAIN;
        if (wait_event_interruptible(&readers, (poll_serial_input(), rhead != rtail)) != 0)
            return -EINTR;
    }
}

int keyboard_poll(struct waitqueue **wq)
{
    poll_serial_input();
    *wq = &readers;
    return rhead != rtail ? POLLIN : 0;
}

void keyboard_set_foreground(uint32_t pid) { fg_pid = pid; }
uint32_t keyboard_get_foreground(void)     { return fg_pid; }

static int kbd_mode = K_XLATE;
static struct file *kbd_owner;              /* the fd that set K_RAW */

int keyboard_set_mode(struct file *f, int mode)
{
    if (mode != K_RAW && mode != K_XLATE)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&kbd_lock);
    kbd_mode = mode;
    kbd_owner = mode == K_RAW ? f : NULL;
#ifdef __x86_64__
    shift = ctrl = 0;
#endif
    rhead = rtail = 0;                      /* don't mix cooked and raw bytes */
    spin_unlock_irqrestore(&kbd_lock, fl);
    return 0;
}

int keyboard_get_mode(void) { return kbd_mode; }

/* The raw-mode owner closed (or died): back to cooked characters. */
void keyboard_release(struct file *f)
{
    if (f == kbd_owner)
        keyboard_set_mode(NULL, K_XLATE);
}

#ifdef __x86_64__
static int e0;

static void keyboard_irq(struct interrupt_frame *f)
{
    (void)f;
    uint8_t raw = inb(KBD_DATA);
    if (kbd_mode == K_RAW) {
        push_char((char)raw);
        return;
    }
    if (raw == 0xE0) {
        e0 = 1;
        return;
    }
    int released = raw & 0x80;
    uint8_t sc = raw & 0x7F;

    if (e0) {
        e0 = 0;
        if (!released) {
            switch (sc) {
            case 0x48: push_char('\033'); push_char('['); push_char('A'); return; /* Up */
            case 0x50: push_char('\033'); push_char('['); push_char('B'); return; /* Down */
            case 0x4D: push_char('\033'); push_char('['); push_char('C'); return; /* Right */
            case 0x4B: push_char('\033'); push_char('['); push_char('D'); return; /* Left */
            }
        }
        return;
    }

    switch (sc) {
    case 0x2A: case 0x36: shift = !released; return;   /* L/R shift */
    case 0x1D: ctrl = !released; return;               /* ctrl */
    case 0x3A: if (!released) caps = !caps; return;     /* caps lock */
    }
    if (released || sc >= 128)
        return;

    char c = map_lower[sc];
    if (!c)
        return;
    if (ctrl && c == 'c') {                             /* ^C -> SIGINT to the foreground process */
        kputs("^C\n");
        struct task *t = fg_pid ? task_find(fg_pid) : NULL;
        if (t)
            task_send_signal(t, SIGINT);
        return;
    }
    int upper = shift;
    if (caps && c >= 'a' && c <= 'z')
        upper = !upper;
    if (upper)
        c = map_upper[sc];
    if (!fb_is_graphics_mode())
        kputc(c);                   /* echo */
    push_char(c);
}
#endif

#ifdef CONFIG_SERIAL
static void serial_irq(struct interrupt_frame *f)
{
    (void)f;
    poll_serial_input();
}
#endif

void keyboard_init(void)
{
#ifdef __x86_64__
    while (inb(0x64) & 1)       /* drain any pending output */
        inb(KBD_DATA);
    irq_install(1, keyboard_irq);
    irq_unmask(1);
#endif
#ifdef CONFIG_SERIAL
    int irq = serial_rx_irq();
    if (irq >= 0) {
        irq_install((uint8_t)irq, serial_irq);
        irq_unmask((uint8_t)irq);
    }
#endif
}

