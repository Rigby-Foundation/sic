/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* powerpc half of a task (kernel/arch/powerpc/task.c). */
#pragma once
#include "types.h"
#include "asm/cpu.h"

struct task;
struct syscall_frame;
typedef void (*task_entry_t)(void *arg);

/* AltiVec register file as ppc_vec_save/ppc_vec_restore (vec.S) lay it out. */
struct ppc_vec_state {
    uint8_t  vr[32][16];
    uint8_t  vscr[16];          /* VSCR in the low word (element 3) */
    uint32_t vrsave, pad[3];
} __attribute__((aligned(16)));

struct arch_task {
    uint32_t sp;                /* saved kernel stack pointer; must be first (switch.S) */
    uint32_t tls;               /* r2 for user code (thread pointer) */
    uint32_t fp_used;           /* FP state saved below is live */
    uint32_t vec_used;          /* likewise for the vector state */
    uint64_t fpr[32];
    uint32_t fpscr, pad;
    struct ppc_vec_state vec;
};

/* Signal frames carry the interrupted code's FP/vector state (kernel/arch/powerpc/signal.c). */
void ppc_fpu_save_for_signal(struct task *t, unsigned long msr);
void ppc_vec_save_for_signal(struct task *t, unsigned long msr);

void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve);
void arch_task_init_user(struct task *t);
void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls);
void arch_switch(struct task *prev, struct task *next);
void arch_switch_first(struct task *next) __attribute__((noreturn));
void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp) __attribute__((noreturn));
uint64_t arch_frame_user_sp(const struct syscall_frame *f);
