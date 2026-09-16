/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "proc/sched.h"
#include "proc/syscall.h"

#define ARG_MAX   64        /* argv + envp entries */
#define ARG_BYTES 8192

/* Start `path` (an ELF in the VFS) as a new user process with stdio on the
 * console; used by the kernel to launch init. argv/envp may be NULL. */
struct task *process_spawn(const char *path, const char *const argv[], const char *const envp[]);

/* Replace the calling process's image. Only returns (with -errno) on failure. */
long process_exec(const char *path, const char *const argv[], const char *const envp[]);

/* Duplicate the calling process; `f` is its syscall frame. Returns the
 * child's pid to the parent (the child sees 0 through its own frame). */
long process_fork(struct syscall_frame *f);
/* clone(2): CLONE_VM|CLONE_THREAD creates a thread sharing mm/fds/sighand;
 * without CLONE_VM it is a fork. */
long process_clone(struct syscall_frame *f, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);

/* Anonymous memory for user processes (mmap/brk). Pages are zeroed.
 * `prot` uses the PROT_* bits from abi.h. */
int  process_map_anon(struct task *t, uint64_t virt, size_t pages, int prot);
int  process_protect(struct task *t, uint64_t virt, size_t pages, int prot);
void process_unmap(struct task *t, uint64_t virt, size_t pages);
