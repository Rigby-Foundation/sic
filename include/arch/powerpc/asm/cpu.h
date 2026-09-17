/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define MAX_CPUS 1

struct task;

/* Per-CPU block. SPRG3 holds its address so exception entry can find the
 * current task's kernel stack. */
struct cpu {
    struct cpu  *self;          /* 0 */
    uint32_t     kernel_sp;     /* 4: current task's kernel stack top */
    uint32_t     index;         /* 8 */
    struct task *current;       /* 12 */
    struct task *idle;          /* 16 */
    volatile int online;
};

extern struct cpu cpus[MAX_CPUS];
extern uint32_t   cpu_count;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mfspr %0, 275" : "=r"(c));   /* SPRG3 */
    return c;
}
void cpu_set_kernel_stack(uint64_t sp);
