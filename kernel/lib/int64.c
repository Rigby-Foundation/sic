/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* 64-bit arithmetic and atomics the compiler expects from libgcc on 32-bit
 * targets (powerpc). Never referenced on x86_64. */
#include "types.h"
#include "spinlock.h"

#if BITS_PER_LONG == 32

static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0) { if (rem) *rem = 0; return 0; }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return udiv64(n, d, NULL); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udiv64(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) != (d < 0);
    uint64_t q = udiv64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, NULL);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    udiv64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}

/* 64-bit atomics: one lock for all of them (signal masks, futex words). */
static spinlock_t atomic64_lock = SPINLOCK_INIT;

uint64_t __atomic_fetch_or_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    unsigned long f = spin_lock_irqsave(&atomic64_lock);
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = old | v;
    spin_unlock_irqrestore(&atomic64_lock, f);
    return old;
}

uint64_t __atomic_fetch_and_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    unsigned long f = spin_lock_irqsave(&atomic64_lock);
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = old & v;
    spin_unlock_irqrestore(&atomic64_lock, f);
    return old;
}

uint64_t __atomic_load_8(const volatile void *p, int order)
{
    (void)order;
    unsigned long f = spin_lock_irqsave(&atomic64_lock);
    uint64_t v = *(const volatile uint64_t *)p;
    spin_unlock_irqrestore(&atomic64_lock, f);
    return v;
}

void __atomic_store_8(volatile void *p, uint64_t v, int order)
{
    (void)order;
    unsigned long f = spin_lock_irqsave(&atomic64_lock);
    *(volatile uint64_t *)p = v;
    spin_unlock_irqrestore(&atomic64_lock, f);
}

#endif
