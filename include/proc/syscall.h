/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "abi/abi.h"

/* Saved by syscall_entry.S. rcx/r11 hold the user rip/rflags on entry (the
 * syscall instruction's own copies); rt_sigreturn overwrites them with the
 * interrupted context's real values and returns through iretq instead of
 * sysret so they survive. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, r11, rcx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

/* Returns 0 to return with sysret, 1 to return with iretq (full frame). */
int syscall_dispatch(struct syscall_frame *f);

void syscall_init_cpu(void);        /* MSRs on the calling CPU */

/* Helpers shared with other syscall implementers (net/socket.c). */
int          user_ok(uint64_t ptr, uint64_t len);   /* user range, fully mapped */
struct file *fd_get(long fd);
int          fd_install(struct file *f);           /* lowest free descriptor, or -EMFILE */
