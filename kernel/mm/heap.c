/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * Kernel heap.
 *
 * Two layers:
 *  - A page-level allocator over the HEAP_BASE virtual range: a bump pointer
 *    plus a small free-run list, each page backed by a fresh PMM frame.
 *  - A slab allocator on top for small objects. Size classes 16..2048 bytes,
 *    each slab is SLAB_PAGES pages: a header followed by objects threaded on
 *    a freelist. Anything bigger is a "large" allocation carved directly
 *    from pages with an inline header.
 *
 * Every heap page has an owner entry (slab pointer or LARGE tag) so kfree()
 * can find its metadata in O(1) without touching the object itself.
 */
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

#define HEAP_PAGES      (HEAP_SIZE / PAGE_SIZE)
#define SLAB_PAGES      4
#define SLAB_BYTES      (SLAB_PAGES * PAGE_SIZE)
#define SLAB_MAGIC      0x534C4142u     /* "SLAB" */
#define LARGE_MAGIC     0x4C52474Cu     /* "LRGL" */
#define OWNER_LARGE     1UL             /* low bit tag; slab pointers are page aligned */
#define MAX_FREE_RUNS   256
#define MAX_SMALL       2048

/* ---- page layer ------------------------------------------------------------ */

struct free_run { uint64_t page; uint64_t count; };

static uint64_t bump_page;                  /* next never-used page index */
static struct free_run free_runs[MAX_FREE_RUNS];
static size_t   free_run_count;
static uint64_t owner[HEAP_PAGES];          /* 0 = unmapped */
static uint64_t pages_mapped;

/* Never taken from interrupt context, so no need to disable interrupts;
 * that also lets heap_free_pages() wait for TLB shootdown acks. */
static spinlock_t heap_lock = SPINLOCK_INIT;

static inline uint64_t page_index(const void *v) { return ((uint64_t)v - HEAP_BASE) >> PAGE_SHIFT; }
static inline void    *page_addr(uint64_t idx)   { return (void *)(HEAP_BASE + (idx << PAGE_SHIFT)); }

static int64_t take_virtual_pages(size_t count)
{
    for (size_t i = 0; i < free_run_count; i++) {
        if (free_runs[i].count < count)
            continue;
        uint64_t page = free_runs[i].page;
        free_runs[i].page  += count;
        free_runs[i].count -= count;
        if (free_runs[i].count == 0)
            free_runs[i] = free_runs[--free_run_count];
        return (int64_t)page;
    }
    if (bump_page + count > HEAP_PAGES)
        return -1;
    uint64_t page = bump_page;
    bump_page += count;
    return (int64_t)page;
}

static void give_virtual_pages(uint64_t page, size_t count)
{
    if (page + count == bump_page) {        /* shrink the bump pointer */
        bump_page = page;
        return;
    }
    for (size_t i = 0; i < free_run_count; i++) {
        if (free_runs[i].page + free_runs[i].count == page) {
            free_runs[i].count += count;
            return;
        }
        if (page + count == free_runs[i].page) {
            free_runs[i].page = page;
            free_runs[i].count += count;
            return;
        }
    }
    if (free_run_count < MAX_FREE_RUNS)
        free_runs[free_run_count++] = (struct free_run){ page, count };
    /* else: leak the virtual range (physical pages were already returned) */
}

static void *alloc_pages_locked(size_t count);
static void free_pages_locked(void *virt, size_t count);

void *heap_alloc_pages(size_t count)
{
    spin_lock(&heap_lock);
    void *p = alloc_pages_locked(count);
    spin_unlock(&heap_lock);
    return p;
}

void heap_free_pages(void *virt, size_t count)
{
    spin_lock(&heap_lock);
    free_pages_locked(virt, count);
    spin_unlock(&heap_lock);
}

static void *alloc_pages_locked(size_t count)
{
    if (count == 0)
        return NULL;
    int64_t first = take_virtual_pages(count);
    if (first < 0)
        return NULL;

    for (size_t i = 0; i < count; i++) {
        uint64_t phys = pmm_alloc_page();
        uint64_t virt = (uint64_t)page_addr(first + i);
        if (!phys || vmm_map_page(virt, phys, PTE_WRITE | PTE_NX | PTE_GLOBAL) != 0) {
            if (phys)
                pmm_free_page(phys);
            free_pages_locked(page_addr(first), i);
            if (i == 0)
                give_virtual_pages(first, count);
            else
                give_virtual_pages(first + i, count - i);
            return NULL;
        }
        owner[first + i] = OWNER_LARGE;     /* provisional; callers override */
        pages_mapped++;
    }
    return page_addr(first);
}

static void free_pages_locked(void *virt, size_t count)
{
    if (!virt || count == 0)
        return;
    uint64_t first = page_index(virt);
    uint64_t phys[64];
    size_t done = 0;

    /* Unmap, make sure no CPU still has the translations, then release the
     * frames — in batches so the phys list stays on the stack. */
    while (done < count) {
        size_t n = count - done < 64 ? count - done : 64;
        for (size_t i = 0; i < n; i++) {
            phys[i] = vmm_unmap_page((uint64_t)page_addr(first + done + i));
            owner[first + done + i] = 0;
        }
        vmm_flush_range((uint64_t)page_addr(first + done), n);
        for (size_t i = 0; i < n; i++)
            if (phys[i]) {
                pmm_free_page(phys[i]);
                pages_mapped--;
            }
        done += n;
    }
    give_virtual_pages(first, count);
}

/* ---- slab layer -------------------------------------------------------------- */

struct slab {
    uint32_t magic;
    uint32_t obj_size;
    uint32_t free_count;
    uint32_t total;
    void    *freelist;
    struct slab *next, *prev;       /* cache partial list */
    struct slab_cache *cache;
};

struct slab_cache {
    uint32_t obj_size;
    struct slab *partial;           /* slabs with at least one free object */
    uint64_t nslabs;
    uint64_t live_objects;
};

static const uint32_t class_sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define NUM_CLASSES (sizeof(class_sizes) / sizeof(class_sizes[0]))
static struct slab_cache caches[NUM_CLASSES];

#define SLAB_HDR_SIZE ((sizeof(struct slab) + 63) & ~63UL)

static struct slab_cache *cache_for(size_t size)
{
    for (size_t i = 0; i < NUM_CLASSES; i++)
        if (size <= class_sizes[i])
            return &caches[i];
    return NULL;
}

static void partial_push(struct slab_cache *c, struct slab *s)
{
    s->prev = NULL;
    s->next = c->partial;
    if (c->partial)
        c->partial->prev = s;
    c->partial = s;
}

static void partial_remove(struct slab_cache *c, struct slab *s)
{
    if (s->prev) s->prev->next = s->next; else c->partial = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static struct slab *slab_create(struct slab_cache *c)
{
    struct slab *s = alloc_pages_locked(SLAB_PAGES);
    if (!s)
        return NULL;

    s->magic = SLAB_MAGIC;
    s->obj_size = c->obj_size;
    s->total = (SLAB_BYTES - SLAB_HDR_SIZE) / c->obj_size;
    s->free_count = s->total;
    s->cache = c;
    s->next = s->prev = NULL;

    /* Thread every object onto the freelist, in address order. */
    uint8_t *obj = (uint8_t *)s + SLAB_HDR_SIZE;
    s->freelist = obj;
    for (uint32_t i = 0; i < s->total; i++) {
        void **link = (void **)(obj + i * c->obj_size);
        *link = (i + 1 < s->total) ? obj + (i + 1) * c->obj_size : NULL;
    }

    uint64_t first = page_index(s);
    for (size_t i = 0; i < SLAB_PAGES; i++)
        owner[first + i] = (uint64_t)s;

    c->nslabs++;
    partial_push(c, s);
    return s;
}

static void slab_destroy(struct slab *s)
{
    struct slab_cache *c = s->cache;
    partial_remove(c, s);
    c->nslabs--;
    s->magic = 0;
    free_pages_locked(s, SLAB_PAGES);
}

static void *slab_alloc(struct slab_cache *c)
{
    struct slab *s = c->partial;
    if (!s && !(s = slab_create(c)))
        return NULL;

    void **obj = s->freelist;
    s->freelist = *obj;
    s->free_count--;
    if (s->free_count == 0)
        partial_remove(c, s);
    c->live_objects++;
    return obj;
}

static void slab_free(struct slab *s, void *ptr)
{
    struct slab_cache *c = s->cache;
    uint64_t off = (uint64_t)ptr - ((uint64_t)s + SLAB_HDR_SIZE);
    if (off % s->obj_size != 0 || off / s->obj_size >= s->total) {
        kprintf("heap: kfree(%p): not an object boundary\n", ptr);
        return;
    }

    *(void **)ptr = s->freelist;
    s->freelist = ptr;
    if (s->free_count++ == 0)
        partial_push(c, s);
    c->live_objects--;

    /* Return a fully free slab unless it's the cache's only spare. */
    if (s->free_count == s->total && (s->next || s->prev))
        slab_destroy(s);
}

/* ---- large allocations ------------------------------------------------------- */

struct large_hdr {
    uint32_t magic;
    uint32_t pad;
    uint64_t pages;
    uint64_t size;
    uint64_t pad2;
};  /* 32 bytes: keeps the returned pointer 16-byte aligned */

static uint64_t large_allocs, bytes_requested;

static void *large_alloc(size_t size)
{
    size_t pages = PAGE_ALIGN_UP(size + sizeof(struct large_hdr)) / PAGE_SIZE;
    struct large_hdr *h = alloc_pages_locked(pages);
    if (!h)
        return NULL;
    h->magic = LARGE_MAGIC;
    h->pages = pages;
    h->size  = size;
    large_allocs++;
    return h + 1;
}

/* ---- public API ---------------------------------------------------------------- */

void heap_init(void)
{
    for (size_t i = 0; i < NUM_CLASSES; i++)
        caches[i].obj_size = class_sizes[i];
    kprintf("heap: %lu MiB virtual at %lx, slab classes 16..%u\n",
            HEAP_SIZE >> 20, HEAP_BASE, class_sizes[NUM_CLASSES - 1]);
}

static void *kmalloc_locked(size_t size)
{
    if (size > MAX_SMALL) {
        void *p = large_alloc(size);
        if (p)
            bytes_requested += size;
        return p;
    }
    struct slab_cache *c = cache_for(size);
    void *p = slab_alloc(c);
    if (p)
        bytes_requested += c->obj_size;     /* charged at class size, matching kfree */
    return p;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    spin_lock(&heap_lock);
    void *p = kmalloc_locked(size);
    spin_unlock(&heap_lock);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

static size_t usable_size(void *ptr, size_t *requested)
{
    uint64_t o = owner[page_index(ptr)];
    if (o == OWNER_LARGE) {
        struct large_hdr *h = (struct large_hdr *)ptr - 1;
        *requested = h->size;
        return h->pages * PAGE_SIZE - sizeof(*h);
    }
    struct slab *s = (struct slab *)o;
    *requested = s->obj_size;   /* best we know */
    return s->obj_size;
}

static void kfree_locked(void *ptr)
{
    uint64_t v = (uint64_t)ptr;
    if (v < HEAP_BASE || v >= HEAP_BASE + HEAP_SIZE) {
        kprintf("heap: kfree(%p): not a heap pointer\n", ptr);
        return;
    }
    uint64_t o = owner[page_index(ptr)];
    if (o == 0) {
        kprintf("heap: kfree(%p): page not allocated\n", ptr);
        return;
    }
    if (o == OWNER_LARGE) {
        struct large_hdr *h = (struct large_hdr *)ptr - 1;
        if (((uint64_t)h & (PAGE_SIZE - 1)) || h->magic != LARGE_MAGIC) {
            kprintf("heap: kfree(%p): bad large header\n", ptr);
            return;
        }
        bytes_requested -= h->size;
        large_allocs--;
        h->magic = 0;
        free_pages_locked(h, h->pages);
        return;
    }
    struct slab *s = (struct slab *)o;
    if (s->magic != SLAB_MAGIC) {
        kprintf("heap: kfree(%p): bad slab magic\n", ptr);
        return;
    }
    bytes_requested -= s->obj_size;
    slab_free(s, ptr);
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    spin_lock(&heap_lock);
    kfree_locked(ptr);
    spin_unlock(&heap_lock);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    spin_lock(&heap_lock);
    size_t requested, cap = usable_size(ptr, &requested);
    void *n = ptr;
    if (!(size <= cap && (size > MAX_SMALL) == (cap > MAX_SMALL))) {
        n = kmalloc_locked(size);
        if (n) {
            memcpy(n, ptr, requested < size ? requested : size);
            kfree_locked(ptr);
        }
    }
    spin_unlock(&heap_lock);
    return n;
}

void heap_get_stats(struct heap_stats *out)
{
    spin_lock(&heap_lock);
    memset(out, 0, sizeof(*out));
    out->pages_mapped = pages_mapped;
    out->large_allocs = large_allocs;
    out->bytes_requested = bytes_requested;
    for (size_t i = 0; i < NUM_CLASSES; i++) {
        out->slabs += caches[i].nslabs;
        out->slab_objects += caches[i].live_objects;
    }
    spin_unlock(&heap_lock);
}

void heap_dump(void)
{
    struct heap_stats st;
    heap_get_stats(&st);
    kprintf("heap: %lu pages mapped, %lu slabs / %lu objects, %lu large, %lu bytes live\n",
            st.pages_mapped, st.slabs, st.slab_objects, st.large_allocs, st.bytes_requested);
    for (size_t i = 0; i < NUM_CLASSES; i++)
        if (caches[i].nslabs)
            kprintf("  class %4u: %lu slabs, %lu live\n",
                    caches[i].obj_size, caches[i].nslabs, caches[i].live_objects);
}
