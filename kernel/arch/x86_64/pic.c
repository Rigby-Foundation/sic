/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "arch/x86_64/pic.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11   /* init + expect ICW4 */
#define ICW4_8086 0x01
#define CMD_EOI   0x20
#define CMD_READ_ISR 0x0B

static inline void io_wait(void) { outb(0x80, 0); }

void pic_init(void)
{
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, IRQ_BASE);     io_wait();   /* master vector offset */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();   /* slave vector offset */
    outb(PIC1_DATA, 4); io_wait();              /* slave on IRQ2 */
    outb(PIC2_DATA, 2); io_wait();              /* slave cascade identity */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, 0xFB);  /* mask all but the cascade line */
    outb(PIC2_DATA, 0xFF);
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) & ~(1 << bit));
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) | (1 << bit));
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, CMD_EOI);
    outb(PIC1_CMD, CMD_EOI);
}

/* IRQ7/IRQ15 can fire spuriously; a real one has its bit set in the ISR. */
int pic_is_spurious(uint8_t irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, CMD_READ_ISR);
        return !(inb(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, CMD_READ_ISR);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, CMD_EOI);    /* master still saw IRQ2 */
            return 1;
        }
    }
    return 0;
}
