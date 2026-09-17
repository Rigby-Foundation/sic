/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The mac-io ESCC (Zilog 8530) serial ports, polled: the kernel log goes to
 * channel A, which QEMU's -serial and a PowerMac's modem port expose. The
 * firmware has already set the line up; we only push bytes. */
#include "drivers/serial.h"
#include "asm/of.h"
#include "endian.h"
#include "asm/irq.h"

#define ESCC_OFFSET   0x13000
#define CH_B_CMD      0x00
#define CH_B_DATA     0x10
#define CH_A_CMD      0x20
#define CH_A_DATA     0x30

static volatile uint8_t *escc;

void serial_init(void)
{
    if (!platform.macio_base)
        return;
    escc = (volatile uint8_t *)(uintptr_t)(platform.macio_base + ESCC_OFFSET);   /* BAT-identity mapped */
}

static void ch_putc(int ch, char c)
{
    if (!escc) return;
    volatile uint8_t *cmd = escc + ch, *data = escc + ch + 0x10;
    for (int spins = 0; spins < 100000; spins++) {
        uint8_t rr0 = mmio_read8(cmd);         /* RR0: bit 2 = Tx buffer empty */
        if (rr0 & 0x04) break;
    }
    mmio_write8(data, (uint8_t)c);
}

void serial_putc(char c)
{
    if (c == '\n')
        ch_putc(CH_A_CMD, '\r');
    ch_putc(CH_A_CMD, c);
}

static void ch_wreg(int ch, uint8_t reg, uint8_t v)
{
    volatile uint8_t *cmd = escc + ch;
    mmio_write8(cmd, reg);
    mmio_write8(cmd, v);
}

int serial_getc(void)
{
    if (!escc) return -1;
    volatile uint8_t *cmd = escc + CH_A_CMD;
    uint8_t rr0 = mmio_read8(cmd);              /* RR0 bit 0: Rx character available */
    if (!(rr0 & 0x01)) {
        ch_wreg(CH_A_CMD, 0, 0x38);             /* reset highest IUS: done with this interrupt */
        return -1;
    }
    uint8_t c = mmio_read8(escc + CH_A_DATA);
    return c;
}

int serial_rx_irq(void)
{
    if (!escc || platform.escc_irq < 0) return -1;
    ch_wreg(CH_A_CMD, 1, 0x10);                 /* WR1: interrupt on every received character */
    ch_wreg(CH_A_CMD, 9, 0x08);                 /* WR9: master interrupt enable */
    ppc_openpic_sense((uint8_t)platform.escc_irq, platform.escc_irq_level, 1);
    return platform.escc_irq;
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
