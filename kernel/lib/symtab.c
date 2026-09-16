/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Kernel symbol table for crash dumps: kernel_syms[] is generated from the
 * linked image by scripts/gensyms.py (two-pass link in the Makefile). */
#include "printf.h"

struct kernel_sym { uint64_t addr; const char *name; };
extern const struct kernel_sym kernel_syms[];
extern const unsigned kernel_sym_count;

const char *ksym_name(uint64_t addr, uint64_t *off)
{
    if (kernel_sym_count == 0 || addr < kernel_syms[0].addr)
        return NULL;
    unsigned lo = 0, hi = kernel_sym_count;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (kernel_syms[mid].addr <= addr) lo = mid; else hi = mid;
    }
    if (addr - kernel_syms[lo].addr > 0x100000)
        return NULL;
    if (off) *off = addr - kernel_syms[lo].addr;
    return kernel_syms[lo].name;
}

/* Print a symbolised address: "0x... <name+0x..>" */
void kprint_sym(uint64_t addr)
{
    uint64_t off;
    const char *n = ksym_name(addr, &off);
    if (n)
        kprintf("%p <%s+0x%lx>", (void *)addr, n, off);
    else
        kprintf("%p", (void *)addr);
}
