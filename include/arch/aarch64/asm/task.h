/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* aarch64 half of a task (kernel/arch/aarch64/task.c). */
#pragma once
#include "types.h"
#include "asm/cpu.h"

struct task;
struct syscall_frame;
typedef void (*task_entry_t)(void *arg);

/* The SIMD/FP register file, as fpsimd_save/fpsimd_restore (entry.S) lay it out. */
struct fpsimd_state {
    uint8_t  v[32][16];
    uint32_t fpsr, fpcr;
    uint32_t pad[2];
} __attribute__((aligned(16)));

struct arch_task {
    uint64_t sp;                /* saved kernel stack pointer; must be first (entry.S) */
    uint64_t tls;               /* TPIDR_EL0: the thread pointer */
    struct fpsimd_state fp;     /* saved around switches for user tasks (the kernel never touches the unit) */
};

void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve);
void arch_task_init_user(struct task *t);
void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls);
void arch_switch(struct task *prev, struct task *next);
void arch_switch_first(struct task *next) __attribute__((noreturn));
void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp) __attribute__((noreturn));
uint64_t arch_frame_user_sp(const struct syscall_frame *f);
void fpsimd_save(struct fpsimd_state *s);      /* entry.S */
void fpsimd_restore(const struct fpsimd_state *s);
