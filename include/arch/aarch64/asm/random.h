/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

static inline uint64_t arch_cycles(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* RNDR (FEAT_RNG) when the CPU has it: 1 and a random word, else 0. */
static inline int arch_hw_random(uint64_t *out)
{
    uint64_t isar0, v, ok;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    if ((isar0 >> 60) == 0) return 0;
    __asm__ volatile("mrs %0, s3_3_c2_c4_0; cset %1, ne" : "=r"(v), "=r"(ok) : : "cc");   /* RNDR; Z set on failure */
    if (!ok) return 0;
    *out = v;
    return 1;
}
