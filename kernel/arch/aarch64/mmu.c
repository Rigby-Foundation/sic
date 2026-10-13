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
#define D_SW_COW    (1UL << 56)     /* software: copy-on-write page */
#define D_ADDR      0x0000fffffffff000UL

extern uint64_t boot_l0[];          /* head.S: the rough first kernel table (TTBR1) */
extern uint64_t kernel_ttbr1;       /* boot.c: the precise one that replaced it */

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
    if (flags & PTE_PCD) d |= D_ATTR(1);                                 /* device nGnRnE */
    else if (flags & PTE_PWT) d |= D_ATTR(3) | D_SH_INNER;               /* normal non-cacheable: frame buffers */
    else d |= D_ATTR(0) | D_SH_INNER;                                    /* normal write-back */
    if (!(flags & PTE_WRITE) || (flags & PTE_COW)) d |= D_AP_RO;
    if (flags & PTE_USER) d |= D_AP_USER | D_NG | D_PXN;                 /* the kernel never executes user pages */
    else d |= D_UXN;
    if (flags & PTE_NX) d |= D_UXN | D_PXN;
    if (flags & PTE_DEV) d |= D_SW_DEV;
    if (flags & PTE_COW) d |= D_SW_COW;
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
    if (d & D_SW_COW) f |= PTE_COW;
    if ((d & D_ATTR(7)) == D_ATTR(1)) f |= PTE_PCD;
    if ((d & D_ATTR(7)) == D_ATTR(3)) f |= PTE_PWT;
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
    kernel_l0 = kernel_ttbr1 ? kernel_ttbr1 : V2P(boot_l0);
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
    if (rc == 0 && (l0_phys == kernel_l0 || l0_phys == (read_sysreg(ttbr0_el1) & D_ADDR))) {
        dsb(ishst);
        invlpg(virt);
    }
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
        uint64_t keep = desc_flags(d) & ~(PTE_WRITE | PTE_NX | PTE_COW);
        if (d & D_SW_COW) {
            if (flags & PTE_WRITE)
                flags = (flags & ~PTE_WRITE) | PTE_COW;
        }
        l3[IDX(virt, 0)] = page_desc(d & D_ADDR, keep | (flags & (PTE_WRITE | PTE_NX | PTE_COW)));
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

/* Normal non-cacheable (PTE_WC), always a new mapping: memory a device reads
 * and writes without snooping the CPU's caches (a GPU's ring, say). */
void *vmm_map_wc(uint64_t phys, size_t size)
{
    uint64_t start = PAGE_ALIGN_DOWN(phys), end = PAGE_ALIGN_UP(phys + size);
    uint64_t virt = __atomic_fetch_add(&mmio_next, end - start, __ATOMIC_SEQ_CST);
    for (uint64_t p = start, v = virt; p < end; p += PAGE_SIZE, v += PAGE_SIZE)
        if (vmm_map_page(v, p, PTE_WRITE | PTE_NX | PTE_WC) != 0)
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
    if ((read_sysreg(ttbr0_el1) & D_ADDR) == l0) {
        write_sysreg(ttbr0_el1, empty_l0);
        __asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
    }
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

/* Copy-on-Write: share every user page of `src` with `dst`.
 * Writable pages are marked read-only with D_SW_COW in both parent and child. */
uint64_t vmm_clone_address_space(uint64_t src)
{
    uint64_t dst = vmm_create_address_space();
    if (!dst) return 0;

    spin_lock(&vmm_lock);
    uint64_t *src_l0 = table_virt(src);
    uint64_t *dst_l0 = table_virt(dst);

    for (int i = 0; i < ENTRIES; i++) {
        if ((src_l0[i] & (D_VALID | D_TABLE)) != (D_VALID | D_TABLE)) continue;
        uint64_t *src_l1 = table_virt(src_l0[i] & D_ADDR);
        uint64_t dst_l1_phys = alloc_table();
        dst_l0[i] = dst_l1_phys | D_VALID | D_TABLE;
        uint64_t *dst_l1 = table_virt(dst_l1_phys);

        for (int j = 0; j < ENTRIES; j++) {
            if ((src_l1[j] & (D_VALID | D_TABLE)) != (D_VALID | D_TABLE)) continue;
            uint64_t *src_l2 = table_virt(src_l1[j] & D_ADDR);
            uint64_t dst_l2_phys = alloc_table();
            dst_l1[j] = dst_l2_phys | D_VALID | D_TABLE;
            uint64_t *dst_l2 = table_virt(dst_l2_phys);

            for (int k = 0; k < ENTRIES; k++) {
                if ((src_l2[k] & (D_VALID | D_TABLE)) != (D_VALID | D_TABLE)) continue;
                uint64_t *src_l3 = table_virt(src_l2[k] & D_ADDR);
                uint64_t dst_l3_phys = alloc_table();
                dst_l2[k] = dst_l3_phys | D_VALID | D_TABLE;
                uint64_t *dst_l3 = table_virt(dst_l3_phys);

                for (int l = 0; l < ENTRIES; l++) {
                    uint64_t e = src_l3[l];
                    if (!(e & D_VALID)) continue;
                    uint64_t phys = e & D_ADDR;

                    if (e & D_SW_DEV) {
                        dst_l3[l] = e;
                        continue;
                    }

                    if (!(e & D_AP_RO) || (e & D_SW_COW)) {
                        /* Writable or already COW: mark both parent and child RO + COW */
                        src_l3[l] = (e | D_AP_RO | D_SW_COW);
                        pmm_page_inc_ref(phys);
                        dst_l3[l] = (e | D_AP_RO | D_SW_COW);
                    } else {
                        /* Plain read-only page (e.g. executable code / rodata) */
                        pmm_page_inc_ref(phys);
                        dst_l3[l] = e;
                    }
                }
            }
        }
    }
    spin_unlock(&vmm_lock);

    /* Broadcast TLBI so all CPUs executing parent drop cached writable entries */
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
    return dst;
}

int vmm_handle_cow(pgd_t pgd, uint64_t virt)
{
    if (!pgd || virt < USER_BASE || virt >= USER_END)
        return -1;

    uint64_t page_virt = PAGE_ALIGN_DOWN(virt);

    spin_lock(&vmm_lock);
    uint64_t *l0 = table_virt(pgd);
    uint64_t *l1 = next_level(l0, IDX(page_virt, 3), 0);
    uint64_t *l2 = l1 ? next_level(l1, IDX(page_virt, 2), 0) : NULL;
    uint64_t *l3 = l2 ? next_level(l2, IDX(page_virt, 1), 0) : NULL;
    if (!l3) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    size_t idx = IDX(page_virt, 0);
    uint64_t e = l3[idx];
    if (!(e & D_VALID)) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    if (!(e & D_SW_COW)) {
        int already_writable = !(e & D_AP_RO);
        spin_unlock(&vmm_lock);
        return already_writable ? 0 : -1;
    }

    uint64_t old_phys = e & D_ADDR;
    uint16_t ref = pmm_page_get_ref(old_phys);

    if (ref <= 1) {
        /* Last owner: simply make the page writable and clear COW bit */
        l3[idx] = (e & ~(D_SW_COW | D_AP_RO));
        spin_unlock(&vmm_lock);
        dsb(ishst);
        invlpg(page_virt);
        return 0;
    }

    /* Allocate private frame and copy */
    uint64_t new_phys = pmm_alloc_page();
    if (!new_phys) {
        spin_unlock(&vmm_lock);
        return -1;
    }

    memcpy(P2V(new_phys), P2V(old_phys), PAGE_SIZE);
    if (!(e & D_UXN)) {
        arch_dcache_clean(P2V(new_phys), PAGE_SIZE);
        arch_icache_invalidate_all();
    }

    pmm_free_page(old_phys);

    uint64_t new_entry = (e & ~D_ADDR & ~(D_SW_COW | D_AP_RO)) | new_phys;
    l3[idx] = new_entry;
    spin_unlock(&vmm_lock);

    dsb(ishst);
    invlpg(page_virt);
    return 0;
}

/* Switch the user half. Local TLB invalidation; no broadcast TLB shootdowns. */
void aarch64_mmu_activate(uint64_t l0)
{
    uint64_t target = (l0 == kernel_l0 ? empty_l0 : l0);
    uint64_t cur = read_sysreg(ttbr0_el1) & D_ADDR;
    if (cur != target) {
        write_sysreg(ttbr0_el1, target);
        __asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
    }
}

void vmm_activate(pgd_t pgd)
{
    aarch64_mmu_activate(pgd);
}
