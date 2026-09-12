/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "zaeboot.h"

#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12

#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

void     pmm_init(const struct zaeboot_info *info);
/* Free firmware boot-services and loader memory. Only call once the kernel
 * runs on its own page tables and has copied everything it needs. */
void     pmm_reclaim_boot_memory(void);
/* Keep [base, base+len) reserved across pmm_reclaim_boot_memory() (e.g. the initrd). */
void     pmm_keep(uint64_t base, uint64_t len);

uint64_t pmm_alloc_page(void);              /* physical address, 0 on failure */
uint64_t pmm_alloc_pages(size_t count);     /* physically contiguous */
uint64_t pmm_alloc_pages_below(size_t count, uint64_t limit);   /* ... with base+size <= limit */
void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, size_t count);

uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_highest_address(void);         /* end of the last usable region */
