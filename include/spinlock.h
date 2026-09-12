/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

typedef struct { volatile int locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked)
            __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1 << 9))
        __asm__ volatile("sti");
}

/* For locks that are also taken from interrupt handlers. */
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
    spin_unlock(l);
    irq_restore(f);
}
