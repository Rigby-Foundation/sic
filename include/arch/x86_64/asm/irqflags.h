/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Interrupt flag save/restore and the spin-wait hint: what spinlocks need. */
#pragma once
#include "types.h"

static inline unsigned long irq_save(void)
{
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(unsigned long flags)
{
    if (flags & (1 << 9))
        __asm__ volatile("sti");
}

static inline void interrupts_enable(void)  { __asm__ volatile("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile("cli"); }
static inline void cpu_relax(void)          { __asm__ volatile("pause"); }
static inline void cpu_halt(void)           { __asm__ volatile("hlt"); }   /* until the next interrupt */
