/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

static inline uint64_t arch_cycles(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0; mftb %1; mftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

static inline int arch_hw_random(uint64_t *out) { (void)out; return 0; }   /* a G4 has none */
