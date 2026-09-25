/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Virtual memory: 4-level paging with an identity map and a higher-half direct map. */
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "asm/apic.h"
#include "asm/smp.h"
#include "asm/cpu.h"

#define ENTRIES 512
#define IDX(virt, level) (((virt) >> (12 + 9 * (level))) & 0x1FF)

#define EFER_MSR 0xC0000080
#define EFER_NXE (1 << 11)

#define IPI_TLB_SHOOTDOWN 240

static uint64_t kernel_pml4;    /* physical */
static int      active;         /* are our tables loaded in CR3? */
static spinlock_t vmm_lock = SPINLOCK_INIT;

extern void pmm_relocate_to_hhdm(void);

/* Page-table pages are touched through the identity map until our tables
 * are live, then through the HHDM. */
static inline uint64_t *table_virt(uint64_t phys)
{
    return active ? (uint64_t *)P2V(phys) : (uint64_t *)phys;
}

static uint64_t alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("vmm: out of memory for page tables\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    memset(table_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* Return the next-level table for entry `idx` of `table`, creating it if needed.
 * New intermediate entries get `tflags` (kernel: W, user: W|U). */
static uint64_t *next_level(uint64_t *table, size_t idx, int create, uint64_t tflags)
{
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create)
            return NULL;
        uint64_t phys = alloc_table();
        table[idx] = phys | PTE_PRESENT | tflags;
    } else if (table[idx] & PTE_HUGE) {
        return NULL;
    }
    return table_virt(table[idx] & PTE_ADDR_MASK);
}

/* Map [0, size) at `virt_base` with 2 MiB pages. */
static void map_direct(uint64_t pml4, uint64_t virt_base, uint64_t size, uint64_t flags)
{
    for (uint64_t off = 0; off < size; off += 0x200000) {
        uint64_t virt = virt_base + off;
        uint64_t *pml4v = table_virt(pml4);
        uint64_t *pdpt  = next_level(pml4v, IDX(virt, 3), 1, PTE_WRITE);
        uint64_t *pd    = next_level(pdpt,  IDX(virt, 2), 1, PTE_WRITE);
        pd[IDX(virt, 1)] = off | PTE_PRESENT | PTE_WRITE | PTE_HUGE | flags;
    }
}

void vmm_init(void)
{
    /* Cover all RAM, and at least 4 GiB so MMIO (framebuffer, LAPIC, ...) stays reachable. */
    uint64_t span = pmm_highest_address();
    if (span < 0x100000000UL)
        span = 0x100000000UL;
    span = (span + 0x3FFFFFFF) & ~0x3FFFFFFFUL;     /* 1 GiB granularity */

    kernel_pml4 = alloc_table();
    map_direct(kernel_pml4, 0,         span, PTE_GLOBAL);
    map_direct(kernel_pml4, HHDM_BASE, span, PTE_GLOBAL | PTE_NX);

    /* Pre-create the PDPTs for the kernel regions so every address space can
     * share them by copying PML4 entries. */
    uint64_t *p = table_virt(kernel_pml4);
    next_level(p, IDX(HEAP_BASE, 3), 1, PTE_WRITE);
    next_level(p, IDX(MMIO_BASE, 3), 1, PTE_WRITE);

    wrmsr(EFER_MSR, rdmsr(EFER_MSR) | EFER_NXE);

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 7);                                  /* PGE */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    write_cr3(kernel_pml4);
    active = 1;
    pmm_relocate_to_hhdm();

    kprintf("vmm: %llu MiB identity + HHDM mapped, pml4 at %llx\n", span >> 20, kernel_pml4);
}

static int map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t tflags)
{
    spin_lock(&vmm_lock);
    uint64_t *pml4 = table_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, IDX(virt, 3), 1, tflags);
    uint64_t *pd   = pdpt ? next_level(pdpt, IDX(virt, 2), 1, tflags) : NULL;
    uint64_t *pt   = pd   ? next_level(pd,   IDX(virt, 1), 1, tflags) : NULL;
    int rc = 0;
    if (!pt)
        rc = -1;            /* would need to split a huge page */
    else if (pt[IDX(virt, 0)] & PTE_PRESENT)
        rc = -2;            /* already mapped */
    else
        pt[IDX(virt, 0)] = (phys & PTE_ADDR_MASK) | PTE_PRESENT | flags;
    spin_unlock(&vmm_lock);
    if (rc == 0)
        invlpg(virt);
    return rc;
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return map_in(kernel_pml4, virt, phys, flags, PTE_WRITE);
}

int vmm_map_user_page(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt < USER_BASE || virt >= USER_END)
        return -3;
    return map_in(pml4, virt, phys, flags | PTE_USER, PTE_WRITE | PTE_USER);
}

static uint64_t unmap_in(uint64_t pml4_phys, uint64_t virt)
{
    spin_lock(&vmm_lock);
    uint64_t *pml4 = table_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, IDX(virt, 3), 0, 0);
    uint64_t *pd   = pdpt ? next_level(pdpt, IDX(virt, 2), 0, 0) : NULL;
    uint64_t *pt   = pd   ? next_level(pd,   IDX(virt, 1), 0, 0) : NULL;
    uint64_t phys = 0;
    if (pt && (pt[IDX(virt, 0)] & PTE_PRESENT)) {
        if (!(pt[IDX(virt, 0)] & PTE_DEV))
            phys = pt[IDX(virt, 0)] & PTE_ADDR_MASK;
        pt[IDX(virt, 0)] = 0;
    }
    spin_unlock(&vmm_lock);
    invlpg(virt);
    return phys;
}

int vmm_protect_user_page(uint64_t pml4_phys, uint64_t virt, uint64_t flags)
{
    spin_lock(&vmm_lock);
    uint64_t *pml4 = table_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, IDX(virt, 3), 0, 0);
    uint64_t *pd   = pdpt ? next_level(pdpt, IDX(virt, 2), 0, 0) : NULL;
    uint64_t *pt   = pd   ? next_level(pd,   IDX(virt, 1), 0, 0) : NULL;
    int rc = -1;
    if (pt && (pt[IDX(virt, 0)] & PTE_PRESENT)) {
        pt[IDX(virt, 0)] = (pt[IDX(virt, 0)] & ~(PTE_WRITE | PTE_NX)) | (flags & (PTE_WRITE | PTE_NX));
        rc = 0;
    }
    spin_unlock(&vmm_lock);
    invlpg(virt);
    return rc;
}

uint64_t vmm_unmap_page(uint64_t virt)                      { return unmap_in(kernel_pml4, virt); }
uint64_t vmm_unmap_user_page(uint64_t pml4, uint64_t virt)  { return unmap_in(pml4, virt); }

static uint64_t translate_in(uint64_t pml4, uint64_t virt)
{
    uint64_t *t = table_virt(pml4);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[IDX(virt, level)];
        if (!(e & PTE_PRESENT))
            return 0;
        if (level == 0)
            return (e & PTE_ADDR_MASK) | (virt & 0xFFF);
        if (e & PTE_HUGE) {
            uint64_t page = level == 1 ? 0x200000UL : 0x40000000UL;
            return (e & PTE_ADDR_MASK & ~(page - 1)) | (virt & (page - 1));
        }
        t = table_virt(e & PTE_ADDR_MASK);
    }
    return 0;
}

uint64_t vmm_translate(uint64_t virt)                 { return translate_in(kernel_pml4, virt); }
uint64_t vmm_translate_in(uint64_t pml4, uint64_t virt) { return translate_in(pml4, virt); }
uint64_t vmm_kernel_pgd(void)                        { return kernel_pml4; }

static uint64_t mmio_next = MMIO_BASE;

void *vmm_map_mmio(uint64_t phys, size_t size)
{
    uint64_t start = PAGE_ALIGN_DOWN(phys);
    uint64_t end   = PAGE_ALIGN_UP(phys + size);
    uint64_t virt  = __atomic_fetch_add(&mmio_next, end - start, __ATOMIC_SEQ_CST);

    for (uint64_t p = start, v = virt; p < end; p += PAGE_SIZE, v += PAGE_SIZE)
        if (vmm_map_page(v, p, PTE_WRITE | PTE_NX | PTE_PCD | PTE_PWT | PTE_GLOBAL) != 0)
            return NULL;
    return (void *)(virt + (phys - start));
}

/* ---- TLB shootdown --------------------------------------------------------- */

static spinlock_t shootdown_lock = SPINLOCK_INIT;
static volatile uint64_t shootdown_virt, shootdown_pages;
static volatile uint32_t shootdown_acks;

/* IPI handler: flush the requested range on this CPU and acknowledge. */
void vmm_shootdown_ipi(void)
{
    uint64_t v = shootdown_virt;
    for (uint64_t i = 0; i < shootdown_pages; i++)
        invlpg(v + i * PAGE_SIZE);
    __atomic_add_fetch(&shootdown_acks, 1, __ATOMIC_SEQ_CST);
}

void vmm_flush_range(uint64_t virt, size_t pages)
{
    for (size_t i = 0; i < pages; i++)
        invlpg(virt + i * PAGE_SIZE);
    if (smp_cpu_count() < 2 || !apic_enabled())
        return;

    /* Other CPUs must be able to take the IPI, so the caller has interrupts
     * enabled; if it doesn't we still flush remotely but can't wait for acks. */
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    int can_wait = flags & (1 << 9);

    spin_lock(&shootdown_lock);
    shootdown_virt = virt;
    shootdown_pages = pages;
    shootdown_acks = 0;
    lapic_broadcast_ipi(IPI_TLB_SHOOTDOWN);
    if (can_wait)
        while (shootdown_acks < smp_cpu_count() - 1)
            cpu_relax();
    spin_unlock(&shootdown_lock);
}

/* ---- address spaces ------------------------------------------------------------ */

uint64_t vmm_create_address_space(void)
{
    uint64_t pml4 = alloc_table();
    uint64_t *src = table_virt(kernel_pml4), *dst = table_virt(pml4);
    dst[0] = src[0];                                /* identity map (kernel only, no U bit) */
    for (int i = 256; i < ENTRIES; i++)             /* HHDM, heap, MMIO */
        dst[i] = src[i];
    return pml4;
}

static void free_table_recursive(uint64_t phys, int level)
{
    uint64_t *t = table_virt(phys);
    for (int i = 0; i < ENTRIES; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT))
            continue;
        if (level == 1) {
            if (!(e & PTE_DEV))
                pmm_free_page(e & PTE_ADDR_MASK);        /* a user frame */
        } else {
            free_table_recursive(e & PTE_ADDR_MASK, level - 1);
        }
    }
    pmm_free_page(phys);
}

void vmm_destroy_address_space(uint64_t pml4)
{
    uint64_t *t = table_virt(pml4);
    for (int i = 1; i < 256; i++)                   /* only the private user part */
        if (t[i] & PTE_PRESENT)
            free_table_recursive(t[i] & PTE_ADDR_MASK, 3);
    /* Still loaded here (a process freeing itself on the way out)? Leave it:
     * the root page may come straight back for the next process, and a
     * switch that sees the same CR3 value would not flush the dead one's TLB. */
    if (read_cr3() == pml4)
        write_cr3(kernel_pml4);
    pmm_free_page(pml4);
}

/* Copy every user page of `src` into a fresh address space (no COW yet). */
/* x86 keeps instruction fetches coherent with stores. */
void arch_dcache_clean(const void *kva, size_t len) { (void)kva; (void)len; }
void arch_icache_invalidate_all(void) {}

uint64_t vmm_clone_address_space(uint64_t src)
{
    uint64_t dst = vmm_create_address_space();
    uint64_t *spml4 = table_virt(src);

    for (int i = 1; i < 256; i++) {
        if (!(spml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = table_virt(spml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < ENTRIES; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = table_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < ENTRIES; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                uint64_t *pt = table_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < ENTRIES; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                    ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    if (pt[l] & PTE_DEV) {
                        uint64_t flags = pt[l] & (PTE_WRITE | PTE_NX | PTE_USER | PTE_DEV);
                        if (vmm_map_user_page(dst, virt, pt[l] & PTE_ADDR_MASK, flags) != 0) {
                            vmm_destroy_address_space(dst);
                            return 0;
                        }
                        continue;
                    }
                    uint64_t phys = pmm_alloc_page();
                    if (!phys) {
                        vmm_destroy_address_space(dst);
                        return 0;
                    }
                    memcpy(P2V(phys), P2V(pt[l] & PTE_ADDR_MASK), PAGE_SIZE);
                    uint64_t flags = pt[l] & (PTE_WRITE | PTE_NX | PTE_USER);
                    if (vmm_map_user_page(dst, virt, phys, flags) != 0) {
                        pmm_free_page(phys);
                        vmm_destroy_address_space(dst);
                        return 0;
                    }
                }
            }
        }
    }
    return dst;
}
