/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

#define IRQ_BASE  32
#define IRQ_COUNT 24                /* ISA 0-15 + I/O APIC GSIs 16-23 */

void idt_init(void);
void idt_load_current(void);        /* lidt on the calling CPU (APs) */
void irq_install(uint8_t irq, irq_handler_t handler);
void irq_use_apic(void);            /* switch masking/EOI from the PIC to the APICs */
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);
void irq_unmask_pci(uint8_t irq);   /* level-triggered, active-low (PCI INTx) */

static inline void interrupts_enable(void)  { __asm__ volatile("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile("cli"); }
