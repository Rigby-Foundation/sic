/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Device interrupts, as drivers see them: a number per line, a handler, mask/unmask. */
#pragma once
#include "types.h"
#include "asm/ptrace.h"
#include "asm/irqflags.h"

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

#define IRQ_BASE  32
#define IRQ_COUNT 24                /* ISA 0-15 + I/O APIC GSIs 16-23 */

void irq_install(uint8_t irq, irq_handler_t handler);
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_unmask_pci(uint8_t irq);   /* level-triggered, active-low (PCI INTx) */
