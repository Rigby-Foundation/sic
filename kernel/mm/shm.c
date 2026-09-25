/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/shmem: shared memory segments (abi/shm.h). A segment is a list of
 * physical pages with a key; files attached to it map the same pages
 * (PTE_DEV: the mapping owns nothing, fork shares it) and the last file
 * frees them. */
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/shm.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "spinlock.h"
#include "string.h"

struct shm_seg {
    uint32_t key;
    size_t pages;
    uint64_t *phys;
    int refs;                   /* open files attached */
    struct shm_seg *next;
};

static struct shm_seg *segments;
static spinlock_t shm_lock;
static uint32_t next_key = 0x5A000001;

static void seg_free(struct shm_seg *s)
{
    for (size_t i = 0; i < s->pages; i++) pmm_free_page(s->phys[i]);
    kfree(s->phys);
    kfree(s);
}

/* shm_lock held; the segment comes out with one more reference */
static struct shm_seg *seg_find(uint32_t key)
{
    for (struct shm_seg *s = segments; s; s = s->next)
        if (s->key == key) { s->refs++; return s; }
    return NULL;
}

static struct shm_seg *seg_create(size_t pages)
{
    struct shm_seg *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->phys = kzalloc(pages * sizeof(uint64_t));
    if (!s->phys) { kfree(s); return NULL; }
    for (s->pages = 0; s->pages < pages; s->pages++) {
        uint64_t p = pmm_alloc_page();
        if (!p) { seg_free(s); return NULL; }
        memset(P2V(p), 0, PAGE_SIZE);
        s->phys[s->pages] = p;
    }
    s->refs = 1;
    spin_lock(&shm_lock);
    s->key = next_key++;
    s->next = segments;
    segments = s;
    spin_unlock(&shm_lock);
    return s;
}

static void seg_put(struct shm_seg *s)
{
    spin_lock(&shm_lock);
    int last = --s->refs == 0;
    if (last)
        for (struct shm_seg **pp = &segments; *pp; pp = &(*pp)->next)
            if (*pp == s) { *pp = s->next; break; }
    spin_unlock(&shm_lock);
    if (last) seg_free(s);
}

static long shm_ioctl(struct file *f, long req, uint64_t arg)
{
    if (!user_ok(arg, sizeof(struct shm_segment))) return -EFAULT;
    struct shm_segment *u = (void *)arg;
    struct shm_seg *s = f->priv_gpu;
    switch (req) {
    case SHM_IOC_CREATE: {
        if (s) return -EBUSY;
        if (u->size == 0 || u->size > (256UL << 20)) return -EINVAL;
        s = seg_create(PAGE_ALIGN_UP(u->size) / PAGE_SIZE);
        if (!s) return -ENOMEM;
        f->priv_gpu = s;
        u->size = s->pages * PAGE_SIZE;
        u->key = s->key;
        return 0;
    }
    case SHM_IOC_ATTACH:
        if (s) return -EBUSY;
        spin_lock(&shm_lock);
        s = seg_find(u->key);
        spin_unlock(&shm_lock);
        if (!s) return -ENOENT;
        f->priv_gpu = s;
        u->size = s->pages * PAGE_SIZE;
        return 0;
    case SHM_IOC_INFO:
        if (!s) return -ENOENT;
        u->size = s->pages * PAGE_SIZE;
        u->key = s->key;
        return 0;
    }
    return -ENOTTY;
}

static int shm_mmap(struct file *f, uint64_t virt, size_t pages, uint64_t off, int prot)
{
    struct shm_seg *s = f->priv_gpu;
    if (!s || off / PAGE_SIZE + pages > s->pages) return -EINVAL;
    struct task *t = task_current();
    uint64_t flags = PTE_DEV | PTE_NX | ((prot & PROT_WRITE) ? PTE_WRITE : 0);
    for (size_t i = 0; i < pages; i++)
        if (vmm_map_user_page(t->mm->pgd, virt + i * PAGE_SIZE, s->phys[off / PAGE_SIZE + i], flags) != 0)
            return -ENOMEM;
    return 0;
}

static void shm_release(struct file *f)
{
    if (f->priv_gpu) seg_put(f->priv_gpu);
}

static const struct dev_ops shm_ops = { .ioctl = shm_ioctl, .mmap = shm_mmap, .release = shm_release };

void shm_init(void)
{
    vfs_mkdev("/dev/shmem", &shm_ops, NULL);
}
