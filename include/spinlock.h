/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "asm/irqflags.h"

typedef struct { volatile int locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked)
            cpu_relax();
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* For locks that are also taken from interrupt handlers. */
static inline unsigned long spin_lock_irqsave(spinlock_t *l)
{
    unsigned long f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long f)
{
    spin_unlock(l);
    irq_restore(f);
}
