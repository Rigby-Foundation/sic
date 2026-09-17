/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define MAX_CPUS 16

/* Segment selectors. Order matters for syscall/sysret (see cpu.c). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct task;

/* Per-CPU block, reachable through %gs. The first fields have fixed offsets
 * used by assembly (syscall entry). */
struct cpu {
    struct cpu  *self;          /* 0  */
    uint64_t     kernel_rsp;    /* 8  current task's kernel stack top */
    uint64_t     user_rsp;      /* 16 scratch for syscall entry */
    uint32_t     index;         /* 24 */
    uint32_t     lapic_id;      /* 28 */
    struct task *current;       /* 32 */
    struct task *idle;          /* 40 */
    volatile int online;
    uint64_t     gdt[7] __attribute__((aligned(16)));
    struct tss   tss __attribute__((aligned(16)));
};

extern struct cpu cpus[MAX_CPUS];
extern uint32_t   cpu_count;    /* CPUs that have been set up (BSP + started APs) */

void        cpu_init(struct cpu *c, uint32_t lapic_id);    /* GDT, TSS, GS base on the calling CPU */
struct cpu *cpu_current(void);
static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}
void        cpu_set_kernel_stack(uint64_t rsp0);
void        syscall_init_cpu(void);     /* MSRs for the syscall instruction, per CPU */

uint64_t rdmsr(uint32_t msr);
void     wrmsr(uint32_t msr, uint64_t v);

#define MSR_FS_BASE 0xC0000100

/* x87/SSE: enabled per CPU; user tasks carry a 512-byte fxsave area. */
void cpu_enable_fpu(void);
extern uint8_t fpu_initial_state[512] __attribute__((aligned(16)));
static inline void fpu_save(void *area)    { __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory"); }
static inline void fpu_restore(const void *area) { __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory"); }
