/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A user address space, shared by the threads of a process. */
#pragma once
#include "types.h"
#include "spinlock.h"

struct file;
/* a writable MAP_SHARED file mapping: a copy, written back to the file on
 * msync, munmap and the address space's end */
struct mm_shared {
    uint64_t virt; size_t pages;
    struct file *f; uint64_t off;
    struct mm_shared *next;
};

struct mm {
    uint64_t pgd;
    uint64_t brk_start, brk_end;    /* program break */
    uint64_t mmap_next;             /* bump pointer for anonymous mmap */
    int      refs;
    spinlock_t lock;                /* brk/mmap bookkeeping */
    char     exe[256];              /* the executable's absolute path (readlink /proc/self/exe) */
    uint64_t arg_start, arg_end;    /* the argv strings on the initial stack (/proc/<pid>/cmdline) */
    struct mm_shared *shared;       /* writable shared file mappings */
};

struct mm *mm_create(uint64_t pgd);
struct mm *mm_get(struct mm *mm);
void       mm_put(struct mm *mm);   /* destroys the address space on the last reference */
/* write a shared mapping's pages in [addr, addr + len) back to its file; drop
 * the mappings wholly inside it if `forget` (munmap) */
void       mm_shared_writeback(struct mm *mm, uint64_t addr, uint64_t len, int forget);
