/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

static inline uint64_t arch_cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* RDSEED, else RDRAND, when the CPU has them: 1 and a random word, else 0. */
static inline int arch_hw_random(uint64_t *out)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    int seed = (b >> 18) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int rand = (c >> 30) & 1;
    for (int tries = 0; tries < 10; tries++) {
        uint64_t v; uint8_t ok = 0;
        if (seed) __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
        else if (rand) __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
        else return 0;
        if (ok) { *out = v; return 1; }
    }
    return 0;
}
