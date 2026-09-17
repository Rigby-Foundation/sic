/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Register frames as the entry code saves them, and how the generic
 * syscall/signal code reads them. */
#pragma once
#include "types.h"

/* Saved by syscall_entry.S. rcx/r11 hold the user rip/rflags on entry (the
 * syscall instruction's own copies); rt_sigreturn overwrites them with the
 * interrupted context's real values and returns through iretq instead of
 * sysret so they survive. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, r11, rcx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

/* Saved by isr.S for exceptions and interrupts. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

/* System V x86_64: number in rax, arguments rdi rsi rdx r10 r8 r9, result in rax. */
#define SYSCALL_NR(f)       ((f)->rax)
#define SYSCALL_ARG1(f)     ((f)->rdi)
#define SYSCALL_ARG2(f)     ((f)->rsi)
#define SYSCALL_ARG3(f)     ((f)->rdx)
#define SYSCALL_ARG4(f)     ((f)->r10)
#define SYSCALL_ARG5(f)     ((f)->r8)
#define SYSCALL_ARG6(f)     ((f)->r9)
#define SYSCALL_SET_RET(f, v) ((f)->rax = (uint64_t)(v))
#define FRAME_FROM_USER(f)  (((f)->cs & 3) != 0)
#define FRAME_PC(f)         ((f)->rip)
#define FRAME_SP(f)         ((f)->rsp)
