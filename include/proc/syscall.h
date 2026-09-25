/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "abi/abi.h"

#include "asm/ptrace.h"

struct task;

/* Returns 0 to return with sysret, 1 to return with iretq (full frame). */
int syscall_dispatch(struct syscall_frame *f);



/* Helpers shared with other syscall implementers (net/socket.c). */
int          user_ok(uint64_t ptr, uint64_t len);   /* user range, fully mapped */
void         task_print_trace(const struct task *t); /* its last system calls, for a post-mortem */
struct file *fd_get(long fd);
int          fd_install(struct file *f);           /* lowest free descriptor, or -EMFILE */
