/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "asm/ptrace.h"

struct task;

struct sigregs {
    struct pt_regs r;
};

void arch_sigregs_from_syscall(struct sigregs *r, const struct syscall_frame *f);
void arch_sigregs_to_syscall(const struct sigregs *r, struct syscall_frame *f);
void arch_sigregs_from_irq(struct sigregs *r, const struct interrupt_frame *f);
void arch_sigregs_to_irq(const struct sigregs *r, struct interrupt_frame *f);
int  arch_signal_setup_frame(struct task *t, int sig, uint64_t fault_addr, uint64_t handler, uint64_t restorer,
                             uint64_t saved_mask, struct sigregs *r);
int  arch_signal_restore_frame(const struct syscall_frame *f, struct sigregs *r, uint64_t *saved_mask);
