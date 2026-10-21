/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "proc/sched.h"
#include "proc/syscall.h"

#define ARG_MAX   4096      /* argv + envp entries (each list) */
#define ARG_BYTES (256 * 1024)   /* their strings: compilers and linkers get long command lines */

struct argpack {
    int    argc, envc;
    char  *argv[ARG_MAX + 1];
    char  *envp[ARG_MAX + 1];
    char   data[ARG_BYTES];
    size_t used;
};

/* Start `path` (an ELF in the VFS) as a new user process with stdio on the
 * console; used by the kernel to launch init. argv/envp may be NULL. */
struct task *process_spawn(const char *path, const char *const argv[], const char *const envp[]);

/* Replace the calling process's image. Only returns (with -errno) on failure. */
long process_exec(const char *path, const char *const argv[], const char *const envp[]);
long process_exec_ap(const char *path, struct argpack *ap);

/* Duplicate the calling process; `f` is its syscall frame. Returns the
 * child's pid to the parent (the child sees 0 through its own frame). */
long process_fork(struct syscall_frame *f);
/* clone(2): CLONE_VM|CLONE_THREAD creates a thread sharing mm/fds/sighand;
 * without CLONE_VM it is a fork. */
long process_clone(struct syscall_frame *f, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);

/* Anonymous memory for user processes (mmap/brk). Pages are zeroed.
 * `prot` uses the PROT_* bits from abi.h. */
int  process_grow_stack(struct task *t, uint64_t addr);   /* demand-grow the user stack; 0 if the fault is fixed */
int  process_map_anon(struct task *t, uint64_t virt, size_t pages, int prot);
int  process_protect(struct task *t, uint64_t virt, size_t pages, int prot);
void process_unmap(struct task *t, uint64_t virt, size_t pages);
/* For /proc: another process's memory, and its resident/total size in pages/bytes. */
struct mm;
size_t process_read_mem(struct mm *mm, uint64_t virt, void *buf, size_t len);
void   process_mem_stats(struct mm *mm, uint64_t *resident_pages, uint64_t *size_bytes);
