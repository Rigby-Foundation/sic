/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* powerpc (32-bit) virtual layout:
 *   0x00000000 - 0x7FFFFFFF   user space (segments 0-7, per process VSIDs)
 *   0x80000000 - 0xBFFFFFFF   PCI memory space, identity mapped by BATs (framebuffer, mac-io)
 *   0xC0000000 - 0xEFFFFFFF   direct map of the first 768 MiB of RAM (BATs)
 *   0xF0000000 - 0xF7FFFFFF   kernel heap
 *   0xF8000000 - 0xFFFFFFFF   uncached MMIO windows (vmm_map_mmio), page-mapped
 * The kernel image lives inside the direct map (loaded at physical 16 MiB). */
#pragma once
#include "types.h"

#define KERNEL_VIRT_BASE 0xC0000000UL
#define HHDM_BASE   0xC0000000UL
#define HHDM_SIZE   0x30000000UL                /* 768 MiB: three 256 MiB BATs */
#define HEAP_BASE   0xF0000000UL
#define HEAP_SIZE   (128UL * 1024 * 1024)
#define MMIO_BASE   0xF8000000UL
#define USER_BASE   0x00001000UL
#define USER_END    0x80000000UL
#define USER_STACK_TOP  0x7FFFF000UL
#define USER_STACK_SIZE (64UL * 1024)          /* mapped at exec; grows on demand up to USER_STACK_MAX */
#define USER_STACK_MAX  (8UL * 1024 * 1024)
#define USER_MMAP_BASE  0x40000000UL
#define PCI_MEM_IDENTITY_BASE 0x80000000UL   /* .. +1 GiB, supervisor only */
#define VMM_TEST_VIRT   0xFFF00000UL            /* self test: a free page-mappable kernel address */

#define P2V(p) ((void *)((uintptr_t)(p) + HHDM_BASE))
#define V2P(v) ((uint64_t)((uintptr_t)(v) - HHDM_BASE))
/* The direct map (BATs) is up from the first instruction. */
#define EARLY_P2V(p) P2V(p)
/* The kernel image is linked inside the direct map. */
#define KERNEL_SYM_PHYS(sym) V2P(sym)

/* Software page table entry bits (what the generic code passes through
 * vmm_*). They are translated to hash PTE bits when a page is entered. */
#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)      /* write-through */
#define PTE_PCD     (1UL << 4)      /* cache inhibited */
#define PTE_HUGE    (1UL << 7)      /* unused */
#define PTE_GLOBAL  (1UL << 8)      /* unused */
#define PTE_DEV     (1UL << 9)      /* device page: never freed, never copied */
#define PTE_NX      (1UL << 10)     /* no-execute (segment-level on 32-bit: recorded only) */
#define PTE_ADDR_MASK 0xFFFFF000UL

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("tlbie %0; sync" : : "r"((uint32_t)virt) : "memory");
}
