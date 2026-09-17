/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* x86_64 half of a task and the context-switch/entry primitives the generic
 * scheduler and process code use (kernel/arch/x86_64/task.c). */
#pragma once
#include "types.h"
#include "asm/cpu.h"

struct task;
struct syscall_frame;
typedef void (*task_entry_t)(void *arg);

struct arch_task {
    uint64_t rsp;               /* saved kernel stack pointer; must be first in struct task (switch.S) */
    uint64_t fs_base;           /* user TLS pointer (arch_prctl) */
    uint8_t  fpu[512] __attribute__((aligned(16)));
};

/* Build the initial kernel stack so the first switch lands in entry(arg).
 * `reserve` bytes at the top of the stack stay untouched (fork's frame). */
void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve);
/* Fresh user register state for exec (FPU defaults, no TLS). */
void arch_task_init_user(struct task *t);
/* The child of fork/clone: a copy of the parent's syscall frame that returns 0;
 * `stack`/`tls` override the user stack pointer and TLS for threads (0 = keep). */
void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls);
/* Switch this CPU from prev to next (sched_lock held, interrupts off). */
void arch_switch(struct task *prev, struct task *next);
/* Leave the bootstrap stack for good and run `next` (an AP entering the scheduler). */
void arch_switch_first(struct task *next) __attribute__((noreturn));
/* exec: activate the new address space on this CPU and drop into ring 3. */
void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp) __attribute__((noreturn));
/* The user stack pointer at the last kernel entry (for clone). */
uint64_t arch_frame_user_sp(const struct syscall_frame *f);
