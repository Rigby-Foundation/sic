/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The 32-bit PowerPC MMU (classic "OEA": BATs + hashed page table).
 *
 * Kernel: the first 768 MiB of RAM are mapped by three BATs at 0xC0000000
 * (set up in head.S), so most kernel memory needs no page table at all.
 * The heap (0xF0000000) and MMIO windows (0xF8000000) are page-mapped
 * through the hash table with the kernel's fixed VSIDs.
 *
 * User: an address space is a context number (VSID base) plus a software
 * page table (a two-level tree of 32-bit entries: physical address + PTE_*
 * flags, the same flags the generic code passes in). Every mapping is
 * inserted into the hash table as it is made; when a PTEG overflows the
 * victim simply faults later and is re-inserted from the software table.
 *
 * vmm_map_page/vmm_translate work on the kernel's software table; the
 * *_user_page variants on a process's. */
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "asm/memlayout.h"
#include "asm/ppc_regs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "proc/sched.h"
#include "proc/mm.h"

extern void pmm_relocate_to_hhdm(void);

#define HTAB_PHYS 0x00400000u
#define HTAB_SIZE 0x00100000u                   /* 1 MiB: 16384 PTEGs */
#define HTAB_MASK ((HTAB_SIZE / 65536) - 1)     /* SDR1 HTABMASK: extra hash bits */

/* Hash PTE (two words) */
#define PTE_V        0x80000000u
#define PTE_H        0x00000040u
#define HPTE_R       0x00000100u
#define HPTE_C       0x00000080u
#define HPTE_W       0x00000040u
#define HPTE_I       0x00000020u
#define HPTE_M       0x00000010u
#define HPTE_G       0x00000008u
#define HPTE_PP_MASK 0x00000003u

struct hpte { uint32_t v, r; };

struct ppc_mm {
    uint32_t ctx;                       /* VSID base = ctx << 4 */
    uint32_t **pgdir;                   /* 1024 entries -> page tables of 1024 sw-PTEs */
};

/* The kernel's own software table (heap, MMIO) and its segment VSIDs. */
static uint32_t *kernel_pgdir[1024];
static struct hpte *htab;
static spinlock_t mmu_lock = SPINLOCK_INIT;

#define KERNEL_VSID_BASE 0x00FFFFF0u         /* segments 12-15: VSIDs 0xFFFFFC..0xFFFFFF */
#define MAX_CONTEXTS 32768
static uint32_t ctx_bitmap[MAX_CONTEXTS / 32];

static inline void sr_write(int i, uint32_t v) { __asm__ volatile("mtsrin %0, %1; isync" : : "r"(v), "r"(i << 28) : "memory"); }
static inline void tlbie(uint32_t va) { __asm__ volatile("tlbie %0; sync" : : "r"(va) : "memory"); }
static inline void tlbia(void) { for (uint32_t va = 0; va < 0x40000; va += 0x1000) tlbie(va); }

static uint32_t vsid_for(uint32_t ctx, uint32_t va)
{
    uint32_t seg = va >> 28;
    if (seg >= 12)
        return KERNEL_VSID_BASE + (seg - 12);
    return ((ctx << 4) | seg) & 0xFFFFFF;
}

/* ---- hash table ------------------------------------------------------------------ */

static uint32_t sw_to_hpte_flags(uint64_t flags)
{
    uint32_t r = HPTE_M;
    if (flags & PTE_PCD) r |= HPTE_I | HPTE_G;
    if (flags & PTE_PWT) r |= HPTE_W;
    if (flags & PTE_USER)
        r |= (flags & PTE_WRITE) ? 2 : 3;           /* PP: 10 = RW both, 11 = RO both */
    else
        r |= (flags & PTE_WRITE) ? 0 : 3;           /* Ks=0: PP 00 = supervisor RW; user none with Kp=1 */
    return r | HPTE_R | HPTE_C;                     /* referenced/changed: we don't track them */
}

static void hpte_insert(uint32_t vsid, uint32_t va, uint32_t phys, uint64_t flags)
{
    uint32_t page = (va >> 12) & 0xFFFF;
    uint32_t hash = (vsid & 0x7FFFF) ^ page;
    uint32_t api = (va >> 22) & 0x3F;
    uint32_t v0 = PTE_V | (vsid << 7) | api;
    uint32_t r0 = (phys & 0xFFFFF000u) | sw_to_hpte_flags(flags);

    unsigned long fl = spin_lock_irqsave(&mmu_lock);
    for (int h = 0; h < 2; h++) {
        uint32_t idx = (h == 0 ? hash : ~hash) & ((HTAB_MASK << 10) | 0x3FF);
        struct hpte *g = &htab[idx * 8];
        uint32_t want = v0 | (h ? PTE_H : 0);
        /* already there (re-insert after eviction, or protection change)? */
        for (int i = 0; i < 8; i++)
            if (g[i].v == want) {
                g[i].v = 0; __asm__ volatile("sync");
                tlbie(va);
                g[i].r = r0; __asm__ volatile("eieio");
                g[i].v = want; __asm__ volatile("sync");
                spin_unlock_irqrestore(&mmu_lock, fl);
                return;
            }
        for (int i = 0; i < 8; i++)
            if (!(g[i].v & PTE_V)) {
                g[i].r = r0; __asm__ volatile("eieio");
                g[i].v = want; __asm__ volatile("sync");
                spin_unlock_irqrestore(&mmu_lock, fl);
                return;
            }
    }
    /* Both groups full: evict a rotating slot of the primary group. */
    static uint32_t victim;
    struct hpte *g = &htab[(hash & ((HTAB_MASK << 10) | 0x3FF)) * 8];
    int i = victim++ & 7;
    g[i].v = 0; __asm__ volatile("sync");
    tlbia();                                         /* we don't know the victim's VA */
    g[i].r = r0; __asm__ volatile("eieio");
    g[i].v = v0; __asm__ volatile("sync");
    spin_unlock_irqrestore(&mmu_lock, fl);
}

static void hpte_remove(uint32_t vsid, uint32_t va)
{
    uint32_t page = (va >> 12) & 0xFFFF;
    uint32_t hash = (vsid & 0x7FFFF) ^ page;
    uint32_t api = (va >> 22) & 0x3F;
    uint32_t v0 = PTE_V | (vsid << 7) | api;
    unsigned long fl = spin_lock_irqsave(&mmu_lock);
    for (int h = 0; h < 2; h++) {
        uint32_t idx = (h == 0 ? hash : ~hash) & ((HTAB_MASK << 10) | 0x3FF);
        struct hpte *g = &htab[idx * 8];
        uint32_t want = v0 | (h ? PTE_H : 0);
        for (int i = 0; i < 8; i++)
            if (g[i].v == want) {
                g[i].v = 0;
                __asm__ volatile("sync");
                tlbie(va);
                spin_unlock_irqrestore(&mmu_lock, fl);
                return;
            }
    }
    tlbie(va);
    spin_unlock_irqrestore(&mmu_lock, fl);
}

/* ---- software page tables ----------------------------------------------------- */

static uint32_t *sw_pte(uint32_t **pgdir, uint32_t va, int create)
{
    uint32_t di = va >> 22;
    if (!pgdir[di]) {
        if (!create) return NULL;
        uint64_t ph = pmm_alloc_page();
        if (!ph) return NULL;
        pgdir[di] = P2V(ph);
        memset(pgdir[di], 0, 4096);
    }
    return &pgdir[di][(va >> 12) & 0x3FF];
}

static int sw_map(uint32_t **pgdir, uint32_t ctx, uint32_t va, uint32_t phys, uint64_t flags)
{
    uint32_t *pte = sw_pte(pgdir, va, 1);
    if (!pte) return -1;
    *pte = (phys & 0xFFFFF000u) | (uint32_t)(flags & 0xFFF) | PTE_PRESENT;
    hpte_insert(vsid_for(ctx, va), va, phys, flags);
    return 0;
}

static uint32_t sw_unmap(uint32_t **pgdir, uint32_t ctx, uint32_t va)
{
    uint32_t *pte = sw_pte(pgdir, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    uint32_t old = *pte;
    *pte = 0;
    hpte_remove(vsid_for(ctx, va), va);
    return old;
}

/* ---- kernel side ---------------------------------------------------------------- */

void vmm_init(void)
{
    htab = P2V(HTAB_PHYS);
    memset(htab, 0, HTAB_SIZE);
    __asm__ volatile("sync");
    __asm__ volatile("mtspr %0, %1; isync" : : "i"(SPRN_SDR1), "r"(HTAB_PHYS | HTAB_MASK));
    /* Kernel segments: fixed VSIDs, supervisor only (Kp = 1). User
     * segments get a dead context until a process runs. */
    for (int i = 0; i < 12; i++) sr_write(i, 0x20000000u | vsid_for(0, (uint32_t)i << 28));
    for (int i = 12; i < 16; i++) sr_write(i, 0x20000000u | vsid_for(0, (uint32_t)i << 28));
    tlbia();
    ctx_bitmap[0] = 1;                                  /* context 0 is nobody's */
    pmm_relocate_to_hhdm();
    kprintf("vmm: %u MiB direct map (BATs), hash table %u KiB at %x, pml4 n/a\n",
            (unsigned)(HHDM_SIZE >> 20), HTAB_SIZE >> 10, HTAB_PHYS);
}

pgd_t vmm_kernel_pgd(void) { return 0; }

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt < HHDM_BASE + HHDM_SIZE) return -1;        /* the direct map is not page-mapped */
    return sw_map(kernel_pgdir, 0, (uint32_t)virt, (uint32_t)phys, flags);
}

uint64_t vmm_unmap_page(uint64_t virt)
{
    uint32_t old = sw_unmap(kernel_pgdir, 0, (uint32_t)virt);
    return old & 0xFFFFF000u;
}

uint64_t vmm_translate(uint64_t virt)
{
    if (virt >= HHDM_BASE && virt < HHDM_BASE + HHDM_SIZE)
        return virt - HHDM_BASE;
    uint32_t *pte = sw_pte(kernel_pgdir, (uint32_t)virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & 0xFFFFF000u) | ((uint32_t)virt & 0xFFF);
}

static uint32_t mmio_next = MMIO_BASE;

void *vmm_map_mmio(uint64_t phys, size_t size)
{
    uint32_t off = (uint32_t)phys & 0xFFF;
    uint32_t base = (uint32_t)phys - off;
    size_t pages = (size + off + 0xFFF) >> 12;
    unsigned long f = spin_lock_irqsave(&mmu_lock);
    uint32_t virt = mmio_next;
    mmio_next += (uint32_t)pages << 12;
    spin_unlock_irqrestore(&mmu_lock, f);
    for (size_t i = 0; i < pages; i++)
        sw_map(kernel_pgdir, 0, virt + ((uint32_t)i << 12), base + ((uint32_t)i << 12), PTE_WRITE | PTE_PCD | PTE_PWT | PTE_DEV);
    return (void *)(uintptr_t)(virt + off);
}

void vmm_flush_range(uint64_t virt, size_t pages)
{
    for (size_t i = 0; i < pages; i++) tlbie((uint32_t)virt + ((uint32_t)i << 12));
}

/* ---- user address spaces -------------------------------------------------------- */

static uint32_t ctx_alloc(void)
{
    unsigned long f = spin_lock_irqsave(&mmu_lock);
    for (uint32_t i = 1; i < MAX_CONTEXTS; i++)
        if (!(ctx_bitmap[i / 32] & (1u << (i % 32)))) {
            ctx_bitmap[i / 32] |= 1u << (i % 32);
            spin_unlock_irqrestore(&mmu_lock, f);
            return i;
        }
    spin_unlock_irqrestore(&mmu_lock, f);
    return 0;
}

static void ctx_free(uint32_t c)
{
    unsigned long f = spin_lock_irqsave(&mmu_lock);
    ctx_bitmap[c / 32] &= ~(1u << (c % 32));
    spin_unlock_irqrestore(&mmu_lock, f);
}

static struct ppc_mm *mm_of(pgd_t pgd) { return (struct ppc_mm *)(uintptr_t)pgd; }

pgd_t vmm_create_address_space(void)
{
    struct ppc_mm *m = kzalloc(sizeof(*m));
    if (!m) return 0;
    m->ctx = ctx_alloc();
    m->pgdir = kzalloc(1024 * sizeof(uint32_t *));
    if (!m->ctx || !m->pgdir) { kfree(m->pgdir); kfree(m); return 0; }
    return (pgd_t)(uintptr_t)m;
}

int vmm_map_user_page(pgd_t pgd, uint64_t virt, uint64_t phys, uint64_t flags)
{
    struct ppc_mm *m = mm_of(pgd);
    if (virt >= USER_END) return -1;
    return sw_map(m->pgdir, m->ctx, (uint32_t)virt, (uint32_t)phys, flags | PTE_USER);
}

uint64_t vmm_unmap_user_page(pgd_t pgd, uint64_t virt)
{
    struct ppc_mm *m = mm_of(pgd);
    uint32_t old = sw_unmap(m->pgdir, m->ctx, (uint32_t)virt);
    if (!old) return 0;
    return (old & PTE_DEV) ? 0 : (old & 0xFFFFF000u);   /* device pages are nobody's frames */
}

int vmm_protect_user_page(pgd_t pgd, uint64_t virt, uint64_t flags)
{
    struct ppc_mm *m = mm_of(pgd);
    uint32_t *pte = sw_pte(m->pgdir, (uint32_t)virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    uint32_t phys = *pte & 0xFFFFF000u;
    uint64_t keep = *pte & (PTE_DEV | PTE_PCD | PTE_PWT);
    *pte = phys | (uint32_t)((flags | keep | PTE_USER) & 0xFFF) | PTE_PRESENT;
    hpte_insert(vsid_for(m->ctx, (uint32_t)virt), (uint32_t)virt, phys, flags | keep | PTE_USER);
    return 0;
}

uint64_t vmm_translate_in(pgd_t pgd, uint64_t virt)
{
    struct ppc_mm *m = mm_of(pgd);
    if (!m) return vmm_translate(virt);
    uint32_t *pte = sw_pte(m->pgdir, (uint32_t)virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & 0xFFFFF000u) | ((uint32_t)virt & 0xFFF);
}

pgd_t vmm_clone_address_space(pgd_t src)
{
    struct ppc_mm *s = mm_of(src);
    pgd_t np = vmm_create_address_space();
    if (!np) return 0;
    struct ppc_mm *d = mm_of(np);
    for (uint32_t di = 0; di < USER_END >> 22; di++) {
        if (!s->pgdir[di]) continue;
        for (uint32_t pi = 0; pi < 1024; pi++) {
            uint32_t pte = s->pgdir[di][pi];
            if (!(pte & PTE_PRESENT)) continue;
            uint32_t va = (di << 22) | (pi << 12);
            uint64_t flags = pte & 0xFFF;
            if (pte & PTE_DEV) {
                if (sw_map(d->pgdir, d->ctx, va, pte & 0xFFFFF000u, flags) != 0) goto fail;
                continue;
            }
            uint64_t ph = pmm_alloc_page();
            if (!ph) goto fail;
            memcpy(P2V(ph), P2V(pte & 0xFFFFF000u), 4096);
            if (sw_map(d->pgdir, d->ctx, va, (uint32_t)ph, flags) != 0) { pmm_free_page(ph); goto fail; }
        }
    }
    return np;
fail:
    vmm_destroy_address_space(np);
    return 0;
}

void vmm_destroy_address_space(pgd_t pgd)
{
    struct ppc_mm *m = mm_of(pgd);
    if (!m) return;
    for (uint32_t di = 0; di < 1024; di++) {
        if (!m->pgdir[di]) continue;
        for (uint32_t pi = 0; pi < 1024; pi++) {
            uint32_t pte = m->pgdir[di][pi];
            if ((pte & PTE_PRESENT) && !(pte & PTE_DEV))
                pmm_free_page(pte & 0xFFFFF000u);
        }
        pmm_free_page(V2P(m->pgdir[di]));
    }
    /* every hash entry of this context is stale: they carry the VSID, so a
     * later context with the same number must not find them */
    unsigned long f = spin_lock_irqsave(&mmu_lock);
    for (uint32_t i = 0; i < HTAB_SIZE / sizeof(struct hpte); i++)
        if ((htab[i].v & PTE_V) && (((htab[i].v >> 7) & 0xFFFFFF) >> 4) == m->ctx)
            htab[i].v = 0;
    __asm__ volatile("sync");
    tlbia();
    spin_unlock_irqrestore(&mmu_lock, f);
    ctx_free(m->ctx);
    kfree(m->pgdir);
    kfree(m);
}

/* Activate a process's segments on this CPU (arch_switch, exec). */
void ppc_mmu_activate(pgd_t pgd)
{
    struct ppc_mm *m = mm_of(pgd);
    uint32_t ctx = m ? m->ctx : 0;
    for (int i = 0; i < 12; i++)
        sr_write(i, vsid_for(ctx, (uint32_t)i << 28));          /* Ks=0, Kp=0: user allowed, PP decides */
    __asm__ volatile("isync");
}

/* A hash miss: if the software table knows the page, re-insert it. */
int ppc_mmu_fault(uint32_t addr, int write, int user)
{
    uint32_t **pgdir = kernel_pgdir;
    uint32_t ctx = 0;
    if (addr < USER_END) {
        struct task *t = task_current();
        if (!t || !t->mm) return -1;
        struct ppc_mm *m = mm_of(t->mm->pgd);
        pgdir = m->pgdir;
        ctx = m->ctx;
    } else if (addr >= HHDM_BASE && addr < HHDM_BASE + HHDM_SIZE) {
        return -1;                                   /* BAT-mapped: a real fault */
    }
    (void)user;
    uint32_t *pte = sw_pte(pgdir, addr, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    if (write && !(*pte & PTE_WRITE)) return -1;
    hpte_insert(vsid_for(ctx, addr), addr, *pte & 0xFFFFF000u, *pte & 0xFFF);
    return 0;
}
