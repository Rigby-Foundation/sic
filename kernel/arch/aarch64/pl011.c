/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The PL011 UART (QEMU virt's console), polled output, interrupt-driven
 * input, behind the same serial_* interface as the x86 16550. */
#include "drivers/serial.h"
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "endian.h"

#define DR    0x00
#define FR    0x18
#define IBRD  0x24
#define FBRD  0x28
#define LCRH  0x2c
#define CR    0x30
#define IMSC  0x38
#define ICR   0x44
#define FR_TXFF (1 << 5)
#define FR_RXFE (1 << 4)

static volatile uint8_t *uart;

void serial_init(void)
{
    uart = P2V(platform.uart_base ? platform.uart_base : 0x09000000);
    mmio_write32(uart + CR, 0);
    mmio_write32(uart + ICR, 0x7ff);
    mmio_write32(uart + LCRH, 0x70);            /* 8 bits, FIFOs on */
    mmio_write32(uart + IMSC, 0);
    mmio_write32(uart + CR, 0x301);             /* UART, TX, RX */
}

void serial_putc(char c)
{
    if (!uart) return;
    if (c == '\n') serial_putc('\r');
    while (mmio_read32(uart + FR) & FR_TXFF) ;
    mmio_write32(uart + DR, (uint8_t)c);
}

void serial_puts(const char *s) { while (*s) serial_putc(*s++); }

int serial_getc(void)
{
    if (!uart || (mmio_read32(uart + FR) & FR_RXFE)) return -1;
    return (int)(mmio_read32(uart + DR) & 0xff);
}

int serial_rx_irq(void)
{
    if (!uart) return -1;
    mmio_write32(uart + IMSC, 1 << 4);          /* RXIM */
    return platform.uart_irq;
}
