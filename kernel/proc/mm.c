/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "proc/mm.h"
#include "mm/vmm.h"
#include "mm/heap.h"

struct mm *mm_create(uint64_t pml4)
{
    struct mm *mm = kzalloc(sizeof(*mm));
    if (!mm)
        return NULL;
    mm->pml4 = pml4;
    mm->refs = 1;
    return mm;
}

struct mm *mm_get(struct mm *mm)
{
    __atomic_add_fetch(&mm->refs, 1, __ATOMIC_SEQ_CST);
    return mm;
}

void mm_put(struct mm *mm)
{
    if (mm && __atomic_sub_fetch(&mm->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        vmm_destroy_address_space(mm->pml4);
        kfree(mm);
    }
}
