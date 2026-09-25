/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define DAIF_I (1UL << 7)

static inline unsigned long irq_save(void)
{
    unsigned long f;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(unsigned long flags)
{
    if (!(flags & DAIF_I))
        __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void interrupts_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void interrupts_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline int  interrupts_enabled(void) { unsigned long f; __asm__ volatile("mrs %0, daif" : "=r"(f)); return !(f & DAIF_I); }
static inline void cpu_relax(void)          { __asm__ volatile("yield" ::: "memory"); }
static inline void cpu_halt(void)           { __asm__ volatile("wfi" ::: "memory"); }
