/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* aarch64 virtual layout (4 KiB granule, 48-bit addresses, TTBR0 user /
 * TTBR1 kernel):
 *   0x0000000000000000 - 0x0000ffffffffffff   user space (TTBR0, per process)
 *   0xffff000000000000 - 0xffff0000ffffffff   direct map of the first 4 GiB of
 *                                             physical space (RAM normal-cacheable,
 *                                             everything below RAM as device memory:
 *                                             the GIC, UART, PCI windows of QEMU virt)
 *   0xffff000100000000 - 0xffff00010fffffff   kernel heap
 *   0xffff000110000000 - ...                  uncached MMIO windows (vmm_map_mmio)
 * The kernel image is linked inside the direct map at RAM base + 2 MiB. */
#pragma once
#include "types.h"

#define KERNEL_VIRT_BASE 0xffff000000000000UL
#define HHDM_BASE   0xffff000000000000UL
#define HHDM_SIZE   0x100000000UL
#define HEAP_BASE   0xffff000100000000UL
#define HEAP_SIZE   (256UL * 1024 * 1024)
#define MMIO_BASE   0xffff000110000000UL
#define USER_BASE   0x0000000000001000UL
#define USER_END    0x0000800000000000UL
#define USER_STACK_TOP  0x00007ffffffff000UL
#define USER_STACK_SIZE (64UL * 1024)          /* mapped at exec; grows on demand up to USER_STACK_MAX */
#define USER_STACK_MAX  (8UL * 1024 * 1024)
#define USER_MMAP_BASE  0x0000100000000000UL
#define VMM_TEST_VIRT   0xffff000120000000UL   /* self test: a free page-mappable kernel address */

#define P2V(p) ((void *)((uintptr_t)(p) + HHDM_BASE))
#define V2P(v) ((uint64_t)((uintptr_t)(v) - HHDM_BASE))
/* The direct map is up before any C runs (head.S). */
#define EARLY_P2V(p) P2V(p)
#define KERNEL_SYM_PHYS(sym) V2P(sym)

/* Software page flags: what the generic code passes through vmm_*. They
 * are the same bits on every architecture; mmu.c turns them into
 * descriptor bits when a page is entered. */
#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)      /* write-through: treated as device */
#define PTE_PCD     (1UL << 4)      /* cache disabled: device memory */
#define PTE_HUGE    (1UL << 7)      /* a block descriptor (2 MiB / 1 GiB) */
#define PTE_GLOBAL  (1UL << 8)      /* unused: the kernel half is global by construction */
#define PTE_DEV     (1UL << 9)      /* device page: never freed, never copied */
#define PTE_NX      (1UL << 10)     /* no-execute */
#define PTE_ADDR_MASK 0x0000fffffffff000UL

static inline void invlpg(uint64_t virt)
{
    /* vae1is: this address, all ASIDs; the kernel half has none, user pages are flushed per process */
    __asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb" : : "r"(virt >> 12) : "memory");   /* broadcast: every CPU */
}
