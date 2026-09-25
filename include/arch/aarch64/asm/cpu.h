/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define MAX_CPUS 8

struct task;

/* Per-CPU block; TPIDR_EL1 holds its address. */
struct cpu {
    struct cpu  *self;
    uint64_t     kernel_sp;     /* the current task's kernel stack top (informational: SP_EL1 tracks it) */
    uint32_t     index;
    uint32_t     pad;
    struct task *current;
    struct task *idle;
    volatile int online;
    uint64_t     mpidr;         /* what PSCI knows it as */
};

extern struct cpu cpus[MAX_CPUS];
extern uint32_t   cpu_count;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}
void cpu_set_kernel_stack(uint64_t sp);
