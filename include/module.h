/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

/* Kernel symbols visible to loadable modules. EXPORT_SYMBOL() puts a
 * {name, address} pair into .ksymtab, which the linker script collects. */
struct ksym {
    const char *name;
    void       *addr;
};

#define EXPORT_SYMBOL(sym) \
    static const struct ksym __ksym_##sym __attribute__((section(".ksymtab"), used)) = { #sym, (void *)&sym }

/* A module is an ET_REL x86_64 object (built with the kernel's flags and
 * `ld.lld -r`) that defines:
 *   const char module_name[];
 *   int  init_module(void);      returns 0 on success
 *   void cleanup_module(void);   optional
 */
struct module {
    char     name[32];
    void    *base;              /* identity-mapped, below 2 GiB */
    size_t   size;
    int    (*init)(void);
    void   (*exit)(void);
    struct module *next;
};

/* Load/unload. Return 0 or -errno. */
int  module_load(const void *image, size_t len, const char *params);
int  module_unload(const char *name);
long module_list(char *buf, size_t len);      /* "name base size\n" lines */
void module_init_ksyms(void);                 /* prints how many symbols are exported */

const struct ksym *ksym_lookup(const char *name);
