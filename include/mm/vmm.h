/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

/* Virtual layout (kernel image itself stays identity mapped at 1 MiB):
 *   0x0000000000000000 - 0x0000000100000000+  identity map of physical memory (PML4[0], kernel only)
 *   0x0000008000000000 - 0x00007FFFFFFFFFFF   user space (PML4[1..255], per process)
 *   0xFFFF800000000000 - ...                  higher-half direct map (HHDM)
 *   0xFFFFC00000000000 - +256 MiB             kernel heap
 *   0xFFFFE00000000000 - ...                  uncached MMIO windows (vmm_map_mmio)
 */
#define HHDM_BASE   0xFFFF800000000000UL
#define HEAP_BASE   0xFFFFC00000000000UL
#define HEAP_SIZE   (256UL * 1024 * 1024)
#define MMIO_BASE   0xFFFFE00000000000UL
#define USER_BASE   0x0000008000000000UL
#define USER_END    0x0000800000000000UL
#define USER_STACK_TOP  0x00007FFFFFFFF000UL
#define USER_STACK_SIZE (64UL * 1024)

#define P2V(p) ((void *)((uint64_t)(p) + HHDM_BASE))
#define V2P(v) ((uint64_t)(v) - HHDM_BASE)

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

void     vmm_init(void);
uint64_t vmm_kernel_pml4(void);

/* Kernel address space (shared by every PML4). Local TLB only; use
 * vmm_flush_range() afterwards when other CPUs may have cached the mapping. */
int      vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);   /* 4 KiB */
uint64_t vmm_unmap_page(uint64_t virt);                                /* returns phys, 0 if unmapped */
uint64_t vmm_translate(uint64_t virt);                                 /* phys or 0 */
void    *vmm_map_mmio(uint64_t phys, size_t size);                     /* uncached, returns virt */
void     vmm_flush_range(uint64_t virt, size_t pages);                 /* TLB shootdown on all CPUs */

/* Per-process address spaces: a PML4 sharing the kernel's mappings plus
 * private user mappings (USER_BASE..USER_END). */
uint64_t vmm_create_address_space(void);
uint64_t vmm_clone_address_space(uint64_t src);          /* deep copy of the user part */
void     vmm_destroy_address_space(uint64_t pml4);      /* frees user frames and tables */
int      vmm_map_user_page(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_unmap_user_page(uint64_t pml4, uint64_t virt);     /* returns phys or 0 */
int      vmm_protect_user_page(uint64_t pml4, uint64_t virt, uint64_t flags); /* W/NX bits */
uint64_t vmm_translate_in(uint64_t pml4, uint64_t virt);

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
