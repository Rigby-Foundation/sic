/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/mouse: struct mouse_event records from a PS/2 mouse (the i8042
 * auxiliary port, 3-byte packets on IRQ 12, x86) or whatever else calls
 * mouse_push (virtio-input). */
#include "drivers/mouse.h"
#include "abi/abi.h"
#include "asm/io.h"
#include "asm/irq.h"
#include "fs/vfs.h"
#include "proc/wait.h"
#include "proc/syscall.h"
#include "spinlock.h"
#include "printf.h"

#define RING 64
static struct mouse_event ring[RING];
static volatile uint32_t rhead, rtail;
static spinlock_t lock = SPINLOCK_INIT;
static struct waitqueue readers = WAITQUEUE_INIT;
void mouse_push(int dx, int dy, unsigned buttons)
{
    struct mouse_event e = { (int16_t)dx, (int16_t)dy, (uint8_t)buttons, 0 };
    uint64_t fl = spin_lock_irqsave(&lock);
    if (rhead - rtail < RING) {
        ring[rhead % RING] = e;
        rhead++;
    }
    spin_unlock_irqrestore(&lock, fl);
    waitqueue_wake_all(&readers);
}

#ifdef __x86_64__
static uint8_t packet[3];
static int npacket;

static int wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return 0; return -1; }
static int wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 0; return -1; }

static int mouse_cmd(uint8_t c)
{
    if (wait_write()) return -1;
    outb(0x64, 0xD4);                       /* next byte goes to the auxiliary device */
    if (wait_write()) return -1;
    outb(0x60, c);
    if (wait_read()) return -1;
    return inb(0x60) == 0xFA ? 0 : -1;      /* ACK */
}

static void mouse_irq(struct interrupt_frame *f)
{
    (void)f;
    if (!(inb(0x64) & 0x21))                /* output buffer full, from the aux port */
        return;
    uint8_t b = inb(0x60);
    if (npacket == 0 && !(b & 0x08))        /* bit 3 is always set in the first byte: resync */
        return;
    packet[npacket++] = b;
    if (npacket < 3)
        return;
    npacket = 0;
    mouse_push(packet[1] - ((packet[0] & 0x10) ? 256 : 0),
               -(packet[2] - ((packet[0] & 0x20) ? 256 : 0)),          /* PS/2 y grows upwards */
               packet[0] & 7);
}
#endif

static long mouse_read(struct file *f, void *buf, size_t len)
{
    struct mouse_event *out = buf;
    size_t max = len / sizeof(*out);
    if (!max) return -EINVAL;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&lock);
        size_t n = 0;
        while (rhead != rtail && n < max) {
            out[n++] = ring[rtail % RING];
            rtail++;
        }
        spin_unlock_irqrestore(&lock, fl);
        if (n)
            return (long)(n * sizeof(*out));
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(&readers, rhead != rtail) != 0)
            return -EINTR;
    }
}

static int mouse_poll(struct file *f, struct waitqueue **wq)
{
    (void)f;
    *wq = &readers;
    return rhead != rtail ? POLLIN : 0;
}

static const struct dev_ops mouse_ops = { .read = mouse_read, .poll = mouse_poll };

void mouse_init(void)
{
    vfs_mkdev("/dev/mouse", &mouse_ops, NULL);
#ifdef __x86_64__
    if (wait_write()) return;
    outb(0x64, 0xA8);                       /* enable the auxiliary port */
    if (wait_write()) return;
    outb(0x64, 0x20);                       /* read the controller configuration byte */
    if (wait_read()) return;
    uint8_t cfg = inb(0x60);
    cfg |= 0x02;                            /* IRQ 12 on */
    cfg &= ~0x20;                           /* aux clock on */
    if (wait_write()) return;
    outb(0x64, 0x60);
    if (wait_write()) return;
    outb(0x60, cfg);
    if (mouse_cmd(0xF6) != 0 || mouse_cmd(0xF4) != 0) {   /* defaults, then stream */
        kprintf("mouse: no PS/2 mouse\n");
        return;
    }
    while (inb(0x64) & 1) inb(0x60);        /* drain */
    irq_install(12, mouse_irq);
    irq_unmask(12);
    kprintf("mouse: PS/2 mouse on irq 12 -> /dev/mouse\n");
#endif
}
