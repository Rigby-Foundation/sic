/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Device interrupts: GICv2 interrupt IDs (kernel/arch/aarch64/gic.c).
 * 0-15 SGIs, 16-31 PPIs (the timer is 27), 32+ SPIs (UART, PCI INTx). */
#pragma once
#include "types.h"
#include "asm/ptrace.h"
#include "asm/irqflags.h"

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

#define IRQ_COUNT 160

void irq_install(uint8_t irq, irq_handler_t handler);
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_unmask_pci(uint8_t irq);   /* level-triggered (PCI INTx) */
