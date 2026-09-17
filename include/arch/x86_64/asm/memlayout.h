/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* x86_64 virtual layout (the kernel image itself stays identity mapped at 1 MiB):
 *   0x0000000000000000 - 0x0000000100000000+  identity map of physical memory (PML4[0], kernel only)
 *   0x0000008000000000 - 0x00007FFFFFFFFFFF   user space (PML4[1..255], per process)
 *   0xFFFF800000000000 - ...                  higher-half direct map (HHDM)
 *   0xFFFFC00000000000 - +256 MiB             kernel heap
 *   0xFFFFE00000000000 - ...                  uncached MMIO windows (vmm_map_mmio)
 */
#pragma once
#include "types.h"

#define HHDM_BASE   0xFFFF800000000000UL
#define HEAP_BASE   0xFFFFC00000000000UL
#define HEAP_SIZE   (256UL * 1024 * 1024)
#define MMIO_BASE   0xFFFFE00000000000UL
#define USER_BASE   0x0000008000000000UL
#define USER_END    0x0000800000000000UL
#define USER_STACK_TOP  0x00007FFFFFFFF000UL
#define USER_STACK_SIZE (64UL * 1024)
#define USER_MMAP_BASE  0x0000010000000000UL    /* 1 TiB: where mmap() hands out ranges */
#define VMM_TEST_VIRT   0xFFFFD00000000000UL    /* self test: a free page-mappable kernel address */

#define P2V(p) ((void *)((uintptr_t)(p) + HHDM_BASE))
#define V2P(v) ((uint64_t)(uintptr_t)(v) - HHDM_BASE)
/* Before vmm_init: the loader left physical memory identity mapped. */
#define EARLY_P2V(p) ((void *)(uintptr_t)(p))
/* The kernel image is linked at its physical address. */
#define KERNEL_SYM_PHYS(sym) ((uint64_t)(uintptr_t)(sym))

/* Page table entry bits: the generic code passes these through vmm_*. */
#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)
#define PTE_PCD     (1UL << 4)
#define PTE_HUGE    (1UL << 7)
#define PTE_GLOBAL  (1UL << 8)
#define PTE_DEV     (1UL << 9)
#define PTE_NX      (1UL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
