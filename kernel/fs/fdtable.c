/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "fs/fdtable.h"
#include "mm/heap.h"

struct fdtable *fdt_create(void)
{
    struct fdtable *t = kzalloc(sizeof(*t));
    if (t)
        t->refs = 1;
    return t;
}

struct fdtable *fdt_clone(struct fdtable *src)
{
    struct fdtable *t = fdt_create();
    if (!t)
        return NULL;
    spin_lock(&src->lock);
    for (int i = 0; i < MAX_FDS; i++)
        if (src->files[i])
            t->files[i] = file_dup(src->files[i]);
    t->cloexec = src->cloexec;
    spin_unlock(&src->lock);
    return t;
}

struct fdtable *fdt_get(struct fdtable *t)
{
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_SEQ_CST);
    return t;
}

void fdt_put(struct fdtable *t)
{
    if (t && __atomic_sub_fetch(&t->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        for (int i = 0; i < MAX_FDS; i++)
            file_close(t->files[i]);
        kfree(t);
    }
}

void fdt_close_on_exec(struct fdtable *t)
{
    if (!t) return;
    spin_lock(&t->lock);
    uint64_t mask = t->cloexec;
    for (int i = 0; i < MAX_FDS; i++) {
        if ((mask & (1ULL << i)) && t->files[i]) {
            struct file *f = t->files[i];
            t->files[i] = NULL;
            file_close(f);
        }
    }
    t->cloexec = 0;
    spin_unlock(&t->lock);
}
