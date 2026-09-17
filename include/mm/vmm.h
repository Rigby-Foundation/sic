/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

/* The virtual memory interface every architecture implements. The layout
 * constants (HHDM_BASE, USER_BASE, ...), the P2V/V2P direct-map helpers and
 * the PTE_* flag bits come from the architecture (asm/memlayout.h); the
 * generic code only ever passes the flags through. A pgd_t names a whole
 * address space (the x86 PML4's physical address, a powerpc context). */
#include "asm/memlayout.h"

typedef uint64_t pgd_t;

void     vmm_init(void);
pgd_t    vmm_kernel_pgd(void);

/* Kernel address space (shared by every PML4). Local TLB only; use
 * vmm_flush_range() afterwards when other CPUs may have cached the mapping. */
int      vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);   /* 4 KiB */
uint64_t vmm_unmap_page(uint64_t virt);                                /* returns phys, 0 if unmapped */
uint64_t vmm_translate(uint64_t virt);                                 /* phys or 0 */
void    *vmm_map_mmio(uint64_t phys, size_t size);                     /* uncached, returns virt */
void     vmm_flush_range(uint64_t virt, size_t pages);                 /* TLB shootdown on all CPUs */

/* Per-process address spaces: a PML4 sharing the kernel's mappings plus
 * private user mappings (USER_BASE..USER_END). */
pgd_t    vmm_create_address_space(void);
pgd_t    vmm_clone_address_space(pgd_t src);            /* deep copy of the user part */
void     vmm_destroy_address_space(pgd_t pgd);          /* frees user frames and tables */
int      vmm_map_user_page(pgd_t pgd, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_unmap_user_page(pgd_t pgd, uint64_t virt);         /* returns phys or 0 */
int      vmm_protect_user_page(pgd_t pgd, uint64_t virt, uint64_t flags); /* W/NX bits */
uint64_t vmm_translate_in(pgd_t pgd, uint64_t virt);

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
