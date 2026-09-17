/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* x86_64 side of the module loader: ELF64 RELA relocations of the small
 * code model. Modules must sit within ±2 GiB of the kernel (linked at 1 MiB)
 * for its 32-bit relocations; the low identity map is RWX. */
#include "module_arch.h"
#include "mm/pmm.h"
#include "abi/abi.h"
#include "printf.h"

#define MODULE_ADDR_LIMIT 0x7FFF0000UL

#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PC64  24

void *arch_module_alloc(size_t pages)
{
    uint64_t base = pmm_alloc_pages_below(pages, MODULE_ADDR_LIMIT);
    return base ? (void *)(uintptr_t)base : NULL;           /* identity mapped */
}

void arch_module_free(void *base, size_t pages) { pmm_free_pages((uint64_t)(uintptr_t)base, pages); }

void arch_module_flush(void *base, size_t size) { (void)base; (void)size; }

int arch_module_reloc(uint8_t *P, uint32_t type, unsigned long S, long A,
                      uint8_t *stubs, size_t *stub_used, const char *symname)
{
    (void)stubs; (void)stub_used;
    int64_t val;
    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_64:
        *(uint64_t *)P = S + (uint64_t)A;
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        val = (int64_t)(S + (uint64_t)A) - (int64_t)(uint64_t)P;
        if (val != (int32_t)val) goto range;
        *(int32_t *)P = (int32_t)val;
        return 0;
    case R_X86_64_32:
        val = (int64_t)(S + (uint64_t)A);
        if ((uint64_t)val >> 32) goto range;
        *(uint32_t *)P = (uint32_t)val;
        return 0;
    case R_X86_64_32S:
        val = (int64_t)(S + (uint64_t)A);
        if (val != (int32_t)val) goto range;
        *(int32_t *)P = (int32_t)val;
        return 0;
    case R_X86_64_PC64:
        *(uint64_t *)P = S + (uint64_t)A - (uint64_t)P;
        return 0;
    default:
        kprintf("module: unsupported relocation type %u against %s\n", type, symname);
        return -ENOEXEC;
    }
range:
    kprintf("module: relocation against %s out of range\n", symname);
    return -ENOEXEC;
}
