/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Physical memory manager: one bit per 4 KiB frame, 1 = used. */
#include "mm/pmm.h"
#include "string.h"
#include "printf.h"
#include "mm/vmm.h"
#include "spinlock.h"
#include "asm/arch.h"

static spinlock_t pmm_lock = SPINLOCK_INIT;

#define MAX_REGIONS 128

extern char _kernel_start[], _kernel_end[];

static struct zaeboot_mmap_entry regions[MAX_REGIONS];
static size_t   region_count;

static uint64_t *bitmap;            /* virtual address */
static uint64_t  bitmap_pages;      /* pages the bitmap covers */
static uint64_t  highest_addr;
static uint64_t  total_pages, used_pages;
static uint64_t  search_hint;       /* first frame that might be free */
static struct { uint64_t base, len; } keep[8];
static size_t keep_count;

static inline int  test_bit(uint64_t i) { return (bitmap[i / 64] >> (i % 64)) & 1; }
static inline void set_bit(uint64_t i)  { bitmap[i / 64] |=  (1ULL << (i % 64)); }
static inline void clr_bit(uint64_t i)  { bitmap[i / 64] &= ~(1ULL << (i % 64)); }

static void mark_range(uint64_t base, uint64_t len, int used)
{
    uint64_t first = used ? PAGE_ALIGN_DOWN(base) : PAGE_ALIGN_UP(base);
    uint64_t end   = used ? PAGE_ALIGN_UP(base + len) : PAGE_ALIGN_DOWN(base + len);
    if (end > bitmap_pages * PAGE_SIZE)
        end = bitmap_pages * PAGE_SIZE;

    for (uint64_t a = first; a < end; a += PAGE_SIZE) {
        uint64_t i = a >> PAGE_SHIFT;
        if (used && !test_bit(i)) {
            set_bit(i);
            used_pages++;
        } else if (!used && test_bit(i)) {
            clr_bit(i);
            used_pages--;
            if (i < search_hint)
                search_hint = i;
        }
    }
}

static uint64_t bitmap_phys_addr;
static int bitmap_is_early;

void pmm_init(const struct zaeboot_info *info)
{
    const struct zaeboot_mmap_entry *mm = (const void *)info->mmap;

    /* Copy the map: it lives in loader memory we'll reclaim later. */
    region_count = info->mmap_count < MAX_REGIONS ? info->mmap_count : MAX_REGIONS;
    memcpy(regions, mm, region_count * sizeof(*regions));

    for (size_t i = 0; i < region_count; i++)
        if (regions[i].type == ZAEBOOT_MEM_USABLE && regions[i].base + regions[i].length > highest_addr)
            highest_addr = regions[i].base + regions[i].length;

    bitmap_pages = highest_addr >> PAGE_SHIFT;
    uint64_t bitmap_bytes = PAGE_ALIGN_UP((bitmap_pages + 7) / 8);

    /* Place the bitmap in the first usable region (above 1 MiB, outside the
     * kernel image) that can hold it. */
    uint64_t kstart = PAGE_ALIGN_DOWN(KERNEL_SYM_PHYS(_kernel_start));
    uint64_t kend   = PAGE_ALIGN_UP(KERNEL_SYM_PHYS(_kernel_end));
    uint64_t bitmap_phys = 0;
    for (size_t i = 0; i < region_count && !bitmap_phys; i++) {
        if (regions[i].type != ZAEBOOT_MEM_USABLE)
            continue;
        uint64_t base = PAGE_ALIGN_UP(regions[i].base);
        uint64_t end  = PAGE_ALIGN_DOWN(regions[i].base + regions[i].length);
        if (base < 0x100000)
            base = 0x100000;
        if (base < kend && end > kstart)
            base = kend;
        if (end > base && end - base >= bitmap_bytes)
            bitmap_phys = base;
    }
    if (!bitmap_phys) {
        kprintf("pmm: no room for frame bitmap\n");
        arch_halt_forever();
    }
    bitmap_phys_addr = bitmap_phys;

    bitmap = EARLY_P2V(bitmap_phys);        /* the direct map comes later on x86 */
    bitmap_is_early = 1;
    memset(bitmap, 0xFF, bitmap_bytes);
    total_pages = bitmap_pages;
    used_pages  = bitmap_pages;
    search_hint = bitmap_pages;

    for (size_t i = 0; i < region_count; i++)
        if (regions[i].type == ZAEBOOT_MEM_USABLE)
            mark_range(regions[i].base, regions[i].length, 0);

    mark_range(0, 0x100000, 1);                     /* legacy low memory, and NULL */
    mark_range(kstart, kend - kstart, 1);           /* ourselves */
    mark_range(bitmap_phys, bitmap_bytes, 1);       /* the bitmap */
}

void pmm_keep(uint64_t base, uint64_t len)
{
    if (keep_count < 8) {
        keep[keep_count].base = base;
        keep[keep_count].len = len;
        keep_count++;
    }
}

void pmm_reclaim_boot_memory(void)
{
    uint64_t kstart = PAGE_ALIGN_DOWN(KERNEL_SYM_PHYS(_kernel_start));
    uint64_t kend   = PAGE_ALIGN_UP(KERNEL_SYM_PHYS(_kernel_end));
    uint64_t freed_before = used_pages;

    for (size_t i = 0; i < region_count; i++) {
        struct zaeboot_mmap_entry *r = &regions[i];
        if (r->type != ZAEBOOT_MEM_BOOTLOADER && r->type != ZAEBOOT_MEM_BOOT_SERVICES)
            continue;
        uint64_t base = r->base, end = r->base + r->length;
        /* Carve the kernel image out of the loader's region. */
        if (base < kend && end > kstart) {
            if (base < kstart)
                mark_range(base, kstart - base, 0);
            if (end > kend)
                mark_range(kend, end - kend, 0);
        } else {
            mark_range(base, end - base, 0);
        }
        r->type = ZAEBOOT_MEM_USABLE;
    }
    mark_range(0, 0x100000, 1);     /* keep low memory reserved regardless */
    for (size_t i = 0; i < keep_count; i++)
        mark_range(keep[i].base, keep[i].len, 1);

    kprintf("pmm: reclaimed %llu KiB of boot memory\n", (freed_before - used_pages) * 4);
}

uint64_t pmm_alloc_pages_below(size_t count, uint64_t limit)
{
    if (count == 0)
        return 0;
    uint64_t max_pages = limit >> PAGE_SHIFT;
    if (max_pages > bitmap_pages)
        max_pages = bitmap_pages;

    spin_lock(&pmm_lock);
    for (uint64_t i = search_hint; i + count <= max_pages; i++) {
        if (test_bit(i))
            continue;
        size_t run = 1;
        while (run < count && !test_bit(i + run))
            run++;
        if (run < count) {
            i += run;
            continue;
        }
        for (size_t k = 0; k < count; k++)
            set_bit(i + k);
        used_pages += count;
        if (count == 1 || i == search_hint)
            search_hint = i + count;
        spin_unlock(&pmm_lock);
        return i << PAGE_SHIFT;
    }
    spin_unlock(&pmm_lock);
    return 0;
}

uint64_t pmm_alloc_pages(size_t count)
{
    return pmm_alloc_pages_below(count, ~0ULL);
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    uint64_t i = phys >> PAGE_SHIFT;
    spin_lock(&pmm_lock);
    for (size_t k = 0; k < count; k++) {
        if (i + k >= bitmap_pages || !test_bit(i + k)) {
            kprintf("pmm: double free of %llx\n", (i + k) << PAGE_SHIFT);
            continue;
        }
        clr_bit(i + k);
        used_pages--;
    }
    if (i < search_hint)
        search_hint = i;
    spin_unlock(&pmm_lock);
}

void pmm_free_page(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

uint64_t pmm_total_pages(void)     { return total_pages; }
uint64_t pmm_used_pages(void)      { return used_pages; }
uint64_t pmm_highest_address(void) { return highest_addr; }

/* Called by the VMM once the higher-half direct map is live so the bitmap
 * is accessed through it rather than the identity map. */
void pmm_relocate_to_hhdm(void)
{
    if (bitmap_is_early && (void *)bitmap != P2V(bitmap_phys_addr))
        bitmap = P2V(bitmap_phys_addr);
    bitmap_is_early = 0;
}
