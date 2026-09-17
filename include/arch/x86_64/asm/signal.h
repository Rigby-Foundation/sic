/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Signal delivery, the machine-dependent half (kernel/arch/x86_64/signal.c). */
#pragma once
#include "types.h"
#include "asm/ptrace.h"

struct task;

/* The user-visible register set: what a handler frame captures and what
 * sigreturn puts back. */
struct sigregs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rbx, rdi, rsi, rdx, rcx, rax;
    uint64_t rip, rflags, rsp;
};

void arch_sigregs_from_syscall(struct sigregs *r, const struct syscall_frame *f);
void arch_sigregs_to_syscall(const struct sigregs *r, struct syscall_frame *f);
void arch_sigregs_from_irq(struct sigregs *r, const struct interrupt_frame *f);
void arch_sigregs_to_irq(const struct sigregs *r, struct interrupt_frame *f);

/* Push the handler frame onto the user stack described by r and rewrite r
 * so that returning runs handler(sig, info, ucontext); the frame remembers
 * *r and saved_mask for sigreturn. -1 if the user stack is unusable. */
int  arch_signal_setup_frame(struct task *t, int sig, uint64_t fault_addr, uint64_t handler, uint64_t restorer,
                             uint64_t saved_mask, struct sigregs *r);
/* Read the frame back at sigreturn; -1 if it is not one of ours. */
int  arch_signal_restore_frame(const struct syscall_frame *f, struct sigregs *r, uint64_t *saved_mask);
