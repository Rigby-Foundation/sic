/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Virtual memory: 4-level tables, 4 KiB granule. TTBR1 holds the kernel's
 * (the direct map head.S built with 1 GiB blocks, the heap, MMIO windows);
 * each process has its own TTBR0 tree for the user half. A pgd_t is the
 * physical address of a level-0 table. */
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "asm/sysreg.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

#define ENTRIES 512
#define IDX(virt, level) (((virt) >> (12 + 9 * (level))) & 0x1FF)

/* Descriptor bits */
#define D_VALID     (1UL << 0)
#define D_TABLE     (1UL << 1)      /* at levels 0-2: next level; at level 3: a page */
#define D_PAGE      (1UL << 1)
#define D_ATTR(i)   ((uint64_t)(i) << 2)
#define D_AP_RO     (1UL << 7)
#define D_AP_USER   (1UL << 6)
#define D_SH_INNER  (3UL << 8)
#define D_AF        (1UL << 10)
#define D_NG        (1UL << 11)
#define D_PXN       (1UL << 53)
#define D_UXN       (1UL << 54)
#define D_SW_DEV    (1UL << 55)     /* software: a device page, never freed */
#define D_ADDR      0x0000fffffffff000UL

extern uint64_t boot_l0[];          /* head.S: the kernel's level-0 table (TTBR1) */

static uint64_t kernel_l0;          /* physical */
static uint64_t empty_l0;           /* TTBR0 while a kernel task runs: nothing in the user half */
static spinlock_t vmm_lock = SPINLOCK_INIT;
extern void pmm_relocate_to_hhdm(void);

static inline uint64_t *table_virt(uint64_t phys) { return (uint64_t *)P2V(phys); }

static uint64_t alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("vmm: out of memory for page tables\n");
        for (;;) __asm__ volatile("wfi");
    }
    memset(table_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* The generic flags -> a level-3 page descriptor. */
static uint64_t page_desc(uint64_t phys, uint64_t flags)
{
    uint64_t d = (phys & D_ADDR) | D_VALID | D_PAGE | D_AF;
    if (flags & (PTE_PCD | PTE_PWT)) d |= D_ATTR(1);                    /* device nGnRnE */
    else d |= D_ATTR(0) | D_SH_INNER;                                    /* normal write-back */
    if (!(flags & PTE_WRITE)) d |= D_AP_RO;
    if (flags & PTE_USER) d |= D_AP_USER | D_NG | D_PXN;                 /* the kernel never executes user pages */
    else d |= D_UXN;
    if (flags & PTE_NX) d |= D_UXN | D_PXN;
    if (flags & PTE_DEV) d |= D_SW_DEV;
    return d;
}

/* ... and back, for the bits the generic code looks at. */
static uint64_t desc_flags(uint64_t d)
{
    uint64_t f = PTE_PRESENT;
    if (!(d & D_AP_RO)) f |= PTE_WRITE;
    if (d & D_AP_USER) f |= PTE_USER;
    if (d & D_UXN) f |= PTE_NX;
    if (d & D_SW_DEV) f |= PTE_DEV;
    if ((d & D_ATTR(7)) == D_ATTR(1)) f |= PTE_PCD;
    return f;
}

static uint64_t *next_level(uint64_t *table, size_t idx, int create)
{
    if (!(table[idx] & D_VALID)) {
        if (!create) return NULL;
        uint64_t phys = alloc_table();
        table[idx] = phys | D_VALID | D_TABLE;
    } else if (!(table[idx] & D_TABLE)) {
        return NULL;                        /* a block */
    }
    return table_virt(table[idx] & D_ADDR);
}

void vmm_init(void)
{
    kernel_l0 = V2P(boot_l0);
    pmm_relocate_to_hhdm();
    empty_l0 = alloc_table();
    write_sysreg(ttbr0_el1, empty_l0);
    __asm__ volatile("isb; tlbi vmalle1is; dsb ish; isb" ::: "memory");
    kprintf("vmm: 4 GiB direct map, kernel level-0 table at %llx\n", kernel_l0);
}

static int map_in(uint64_t l0_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    spin_lock(&vmm_lock);
    uint64_t *l0 = table_virt(l0_phys);
    uint64_t *l1 = next_level(l0, IDX(virt, 3), 1);
    uint64_t *l2 = l1 ? next_level(l1, IDX(virt, 2), 1) : NULL;
    uint64_t *l3 = l2 ? next_level(l2, IDX(virt, 1), 1) : NULL;
    int rc = 0;
    if (!l3) rc = -1;                       /* would need to split a block */
    else if (l3[IDX(virt, 0)] & D_VALID) rc = -2;
    else l3[IDX(virt, 0)] = page_desc(phys, flags);
    spin_unlock(&vmm_lock);
    if (rc == 0) { dsb(ishst); invlpg(virt); }
    return rc;
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return map_in(kernel_l0, virt, phys, flags & ~PTE_USER);
}

int vmm_map_user_page(uint64_t l0, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt < USER_BASE || virt >= USER_END) return -3;
    return map_in(l0, virt, phys, flags | PTE_USER);
}

static uint64_t unmap_in(uint64_t l0_phys, uint64_t virt)
{
    spin_lock(&vmm_lock);
    uint64_t *l0 = table_virt(l0_phys);
    uint64_t *l1 = next_level(l0, IDX(virt, 3), 0);
    uint64_t *l2 = l1 ? next_level(l1, IDX(virt, 2), 0) : NULL;
    uint64_t *l3 = l2 ? next_level(l2, IDX(virt, 1), 0) : NULL;
    uint64_t phys = 0;
    if (l3 && (l3[IDX(virt, 0)] & D_VALID)) {
        if (!(l3[IDX(virt, 0)] & D_SW_DEV)) phys = l3[IDX(virt, 0)] & D_ADDR;
        l3[IDX(virt, 0)] = 0;
    }
    spin_unlock(&vmm_lock);
    invlpg(virt);
    return phys;
}

int vmm_protect_user_page(uint64_t l0_phys, uint64_t virt, uint64_t flags)
{
    spin_lock(&vmm_lock);
    uint64_t *l0 = table_virt(l0_phys);
    uint64_t *l1 = next_level(l0, IDX(virt, 3), 0);
    uint64_t *l2 = l1 ? next_level(l1, IDX(virt, 2), 0) : NULL;
    uint64_t *l3 = l2 ? next_level(l2, IDX(virt, 1), 0) : NULL;
    int rc = -1;
    if (l3 && (l3[IDX(virt, 0)] & D_VALID)) {
        uint64_t d = l3[IDX(virt, 0)];
        uint64_t keep = desc_flags(d) & ~(PTE_WRITE | PTE_NX);
        l3[IDX(virt, 0)] = page_desc(d & D_ADDR, keep | (flags & (PTE_WRITE | PTE_NX)));
        rc = 0;
    }
    spin_unlock(&vmm_lock);
    invlpg(virt);
    return rc;
}

uint64_t vmm_unmap_page(uint64_t virt)                    { return unmap_in(kernel_l0, virt); }
uint64_t vmm_unmap_user_page(uint64_t l0, uint64_t virt)  { return unmap_in(l0, virt); }

static uint64_t translate_in(uint64_t l0_phys, uint64_t virt)
{
    uint64_t *t = table_virt(l0_phys);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[IDX(virt, level)];
        if (!(e & D_VALID)) return 0;
        if (level == 0) return (e & D_ADDR) | (virt & 0xFFF);
        if (!(e & D_TABLE)) {
            uint64_t page = level == 1 ? 0x200000UL : 0x40000000UL;
            return (e & D_ADDR & ~(page - 1)) | (virt & (page - 1));
        }
        t = table_virt(e & D_ADDR);
    }
    return 0;
}

/* Kernel addresses walk TTBR1's tree, user ones the process's. */
uint64_t vmm_translate(uint64_t virt)
{
    return translate_in(virt >= HHDM_BASE ? kernel_l0 : (uint64_t)read_sysreg(ttbr0_el1) & D_ADDR, virt);
}
uint64_t vmm_translate_in(uint64_t l0, uint64_t virt) { return translate_in(virt >= HHDM_BASE ? kernel_l0 : l0, virt); }
uint64_t vmm_kernel_pgd(void)                         { return kernel_l0; }

static uint64_t mmio_next = MMIO_BASE;

void *vmm_map_mmio(uint64_t phys, size_t size)
{
    uint64_t start = PAGE_ALIGN_DOWN(phys), end = PAGE_ALIGN_UP(phys + size);
    if (end <= HHDM_SIZE && start < 0x40000000UL)          /* the direct map already has it as device memory */
        return P2V(phys);
    uint64_t virt = __atomic_fetch_add(&mmio_next, end - start, __ATOMIC_SEQ_CST);
    for (uint64_t p = start, v = virt; p < end; p += PAGE_SIZE, v += PAGE_SIZE)
        if (vmm_map_page(v, p, PTE_WRITE | PTE_NX | PTE_PCD) != 0)
            return NULL;
    return (void *)(virt + (phys - start));
}

/* TLB invalidations are broadcast on aarch64 (the *is forms): no IPI. */
void vmm_flush_range(uint64_t virt, size_t pages)
{
    for (size_t i = 0; i < pages; i++) invlpg(virt + i * PAGE_SIZE);
}

/* ---- address spaces (the user half: TTBR0) ---------------------------------- */

uint64_t vmm_create_address_space(void)
{
    return alloc_table();
}

static void free_table_recursive(uint64_t phys, int level)
{
    uint64_t *t = table_virt(phys);
    for (int i = 0; i < ENTRIES; i++) {
        uint64_t e = t[i];
        if (!(e & D_VALID)) continue;
        if (level == 0) { if (!(e & D_SW_DEV)) pmm_free_page(e & D_ADDR); }
        else free_table_recursive(e & D_ADDR, level - 1);
    }
    pmm_free_page(phys);
}

void vmm_destroy_address_space(uint64_t l0)
{
    free_table_recursive(l0, 3);
    /* No ASIDs: a CPU may still cache this tree's translations, and the
     * next address space can get the same root page back, which a switch
     * would take for "no change" and not flush. Drop them everywhere now. */
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

void arch_dcache_clean(const void *kva, size_t len)
{
    uintptr_t a = (uintptr_t)kva & ~63UL, e = (uintptr_t)kva + len;
    for (uintptr_t p = a; p < e; p += 64) __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

/* By VA would miss lines a VIPT cache holds under the user's addresses. */
void arch_icache_invalidate_all(void)
{
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
}

/* Copy every user page of `src` into a fresh address space (no COW yet). */
uint64_t vmm_clone_address_space(uint64_t src)
{
    uint64_t dst = vmm_create_address_space();
    int code = 0;           /* executable pages copied: the frames may hold another program's code in the I-cache */
    uint64_t *l0 = table_virt(src);
    for (int i = 0; i < ENTRIES; i++) {
        if (!(l0[i] & D_VALID)) continue;
        uint64_t *l1 = table_virt(l0[i] & D_ADDR);
        for (int j = 0; j < ENTRIES; j++) {
            if (!(l1[j] & D_VALID)) continue;
            uint64_t *l2 = table_virt(l1[j] & D_ADDR);
            for (int k = 0; k < ENTRIES; k++) {
                if (!(l2[k] & D_VALID)) continue;
                uint64_t *l3 = table_virt(l2[k] & D_ADDR);
                for (int l = 0; l < ENTRIES; l++) {
                    uint64_t e = l3[l];
                    if (!(e & D_VALID)) continue;
                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    uint64_t flags = desc_flags(e) & (PTE_WRITE | PTE_NX | PTE_USER | PTE_DEV | PTE_PCD);
                    if (e & D_SW_DEV) {
                        if (vmm_map_user_page(dst, virt, e & D_ADDR, flags) != 0) { vmm_destroy_address_space(dst); return 0; }
                        continue;
                    }
                    uint64_t phys = pmm_alloc_page();
                    if (!phys) { vmm_destroy_address_space(dst); return 0; }
                    memcpy(P2V(phys), P2V(e & D_ADDR), PAGE_SIZE);
                    if (!(e & D_UXN)) { arch_dcache_clean(P2V(phys), PAGE_SIZE); code = 1; }
                    if (vmm_map_user_page(dst, virt, phys, flags) != 0) { pmm_free_page(phys); vmm_destroy_address_space(dst); return 0; }
                }
            }
        }
    }
    if (code) arch_icache_invalidate_all();
    return dst;
}

/* Switch the user half. No ASIDs yet: flush everything. */
void aarch64_mmu_activate(uint64_t l0)
{
    write_sysreg(ttbr0_el1, l0 == kernel_l0 ? empty_l0 : l0);
    __asm__ volatile("isb; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}
