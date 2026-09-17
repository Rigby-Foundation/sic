/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define MSR_EE  (1UL << 15)
#define MSR_PR  (1UL << 14)
#define MSR_VEC (1UL << 25)
#define MSR_FP  (1UL << 13)
#define MSR_ME  (1UL << 12)
#define MSR_IR  (1UL << 5)
#define MSR_DR  (1UL << 4)
#define MSR_RI  (1UL << 1)

static inline unsigned long mfmsr(void) { unsigned long v; __asm__ volatile("mfmsr %0" : "=r"(v)); return v; }
static inline void mtmsr(unsigned long v) { __asm__ volatile("mtmsr %0; isync" : : "r"(v) : "memory"); }

static inline unsigned long irq_save(void)
{
    unsigned long m = mfmsr();
    mtmsr(m & ~MSR_EE);
    return m;
}

static inline void irq_restore(unsigned long flags)
{
    if (flags & MSR_EE)
        mtmsr(mfmsr() | MSR_EE);
}

static inline void interrupts_enable(void)  { mtmsr(mfmsr() | MSR_EE); }
static inline void interrupts_disable(void) { mtmsr(mfmsr() & ~MSR_EE); }
static inline void cpu_relax(void)          { __asm__ volatile("or 1,1,1; or 2,2,2" ::: "memory"); }  /* low priority hint */
static inline void cpu_halt(void)           { __asm__ volatile("sync; isync" ::: "memory"); }         /* no real halt: idle spins */
