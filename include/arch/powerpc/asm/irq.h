/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Device interrupts: OpenPIC sources on a PowerMac (kernel/arch/powerpc/openpic.c). */
#pragma once
#include "types.h"
#include "asm/ptrace.h"
#include "asm/irqflags.h"

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

#define IRQ_COUNT 64

void irq_install(uint8_t irq, irq_handler_t handler);
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_unmask_pci(uint8_t irq);   /* level-triggered, active-low (PCI INTx) */
void ppc_openpic_sense(uint8_t irq, int level, int active_high);   /* before irq_unmask */
