/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_getc(void);             /* next received byte, or -1 if none is waiting */
int  serial_rx_irq(void);           /* enable receive interrupts; returns the IRQ, or -1 if polled only */
