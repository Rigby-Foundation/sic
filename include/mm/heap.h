/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void  heap_init(void);

void *kmalloc(size_t size);             /* 16-byte aligned, NULL on failure */
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

/* Page-granular allocations from the heap's virtual range (page aligned). */
void *heap_alloc_pages(size_t count);
void  heap_free_pages(void *virt, size_t count);

/* Kernel stacks: physically contiguous pages in the direct map, so that
 * exception entry can find them before address translation is on (powerpc). */
void *kstack_alloc(size_t pages);
void  kstack_free(void *virt, size_t pages);

struct heap_stats {
    uint64_t pages_mapped;      /* backing pages currently mapped */
    uint64_t slabs;             /* live slabs */
    uint64_t slab_objects;      /* live objects in slabs */
    uint64_t large_allocs;      /* live large allocations */
    uint64_t bytes_requested;   /* live bytes: class size for slab objects, exact for large */
};
void heap_get_stats(struct heap_stats *out);
void heap_dump(void);
