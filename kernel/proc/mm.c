/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "proc/mm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "asm/memlayout.h"

#define PAGE 4096ULL

void mm_shared_writeback(struct mm *mm, uint64_t addr, uint64_t len, int forget)
{
    for (struct mm_shared **pp = &mm->shared; *pp; ) {
        struct mm_shared *s = *pp;
        uint64_t s_end = s->virt + s->pages * PAGE, end = addr + len;
        if (s_end <= addr || s->virt >= end) { pp = &s->next; continue; }
        uint64_t size = s->f->node ? s->f->node->size : 0;
        uint64_t from = s->virt > addr ? s->virt : addr, to = s_end < end ? s_end : end;
        for (uint64_t va = from & ~(PAGE - 1); va < to; va += PAGE) {
            uint64_t foff = s->off + (va - s->virt);
            if (foff >= size) break;                        /* never past the end of the file */
            uint64_t phys = vmm_translate_in(mm->pgd, va);
            if (!phys) continue;                            /* unmapped already */
            size_t n = size - foff < PAGE ? (size_t)(size - foff) : (size_t)PAGE;
            file_pwrite(s->f, P2V(phys & ~(PAGE - 1)), n, foff);
        }
        if (forget && s->virt >= addr && s_end <= end) {
            *pp = s->next;
            file_close(s->f);
            kfree(s);
            continue;
        }
        pp = &s->next;
    }
}

struct mm *mm_create(uint64_t pgd)
{
    struct mm *mm = kzalloc(sizeof(*mm));
    if (!mm)
        return NULL;
    mm->pgd = pgd;
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
        if (mm->shared) mm_shared_writeback(mm, 0, ~0ULL >> 1, 1);
        vmm_destroy_address_space(mm->pgd);
        kfree(mm);
    }
}
