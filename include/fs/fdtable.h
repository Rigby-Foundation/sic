/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* File descriptor table, shared by the threads of a process. */
#pragma once
#include "fs/vfs.h"
#include "spinlock.h"

struct fdtable {
    struct file *files[MAX_FDS];
    int refs;
    spinlock_t lock;
};

struct fdtable *fdt_create(void);
struct fdtable *fdt_clone(struct fdtable *src);     /* fork: same files, new table */
struct fdtable *fdt_get(struct fdtable *t);         /* threads: share */
void            fdt_put(struct fdtable *t);         /* closes everything on the last reference */
