/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A user address space, shared by the threads of a process. */
#pragma once
#include "types.h"
#include "spinlock.h"

struct mm {
    uint64_t pml4;
    uint64_t brk_start, brk_end;    /* program break */
    uint64_t mmap_next;             /* bump pointer for anonymous mmap */
    int      refs;
    spinlock_t lock;                /* brk/mmap bookkeeping */
};

struct mm *mm_create(uint64_t pml4);
struct mm *mm_get(struct mm *mm);
void       mm_put(struct mm *mm);   /* destroys the address space on the last reference */
