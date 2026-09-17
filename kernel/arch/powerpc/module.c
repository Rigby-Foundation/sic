/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PowerPC side of the module loader: ELF32 RELA relocations of the SysV
 * ppc32 ABI (non-PIC code, -mcpu=7450). Module memory is ordinary pages in
 * the direct map, which can be anywhere in RAM while the kernel sits at
 * 16 MiB, so a `bl` to a kernel function is usually out of its ±32 MiB
 * reach: those get a stub in the module (lis/ori/mtctr/bctr) and the branch
 * is pointed at the stub instead. */
#include "module_arch.h"
#include "mm/pmm.h"
#include "asm/memlayout.h"
#include "abi/abi.h"
#include "printf.h"

#define R_PPC_NONE       0
#define R_PPC_ADDR32     1
#define R_PPC_ADDR24     2
#define R_PPC_ADDR16     3
#define R_PPC_ADDR16_LO  4
#define R_PPC_ADDR16_HI  5
#define R_PPC_ADDR16_HA  6
#define R_PPC_ADDR14     7
#define R_PPC_REL24      10
#define R_PPC_REL14      11
#define R_PPC_REL32      26
#define R_PPC_PLTREL24   18
#define R_PPC_LOCAL24PC  23

void *arch_module_alloc(size_t pages)
{
    uint64_t base = pmm_alloc_pages(pages);
    return base ? P2V(base) : NULL;
}

void arch_module_free(void *base, size_t pages) { pmm_free_pages(V2P(base), pages); }

void arch_module_flush(void *base, size_t size)
{
    uintptr_t a = (uintptr_t)base & ~31UL, e = (uintptr_t)base + size;
    for (uintptr_t p = a; p < e; p += 32) __asm__ volatile("dcbst 0,%0" : : "r"(p) : "memory");
    __asm__ volatile("sync");
    for (uintptr_t p = a; p < e; p += 32) __asm__ volatile("icbi 0,%0" : : "r"(p) : "memory");
    __asm__ volatile("sync; isync");
}

static inline uint16_t ha16(uint32_t v) { return (uint16_t)((v + 0x8000) >> 16); }

int arch_module_reloc(uint8_t *P, uint32_t type, unsigned long S, long A,
                      uint8_t *stubs, size_t *stub_used, const char *symname)
{
    uint32_t val = (uint32_t)S + (uint32_t)A;
    uint32_t *insn = (uint32_t *)P;
    int32_t rel;

    switch (type) {
    case R_PPC_NONE:
        return 0;
    case R_PPC_ADDR32:
        *(uint32_t *)P = val;
        return 0;
    case R_PPC_REL32:
        *(uint32_t *)P = val - (uint32_t)(uintptr_t)P;
        return 0;
    case R_PPC_ADDR16_LO:
        *(uint16_t *)P = (uint16_t)val;
        return 0;
    case R_PPC_ADDR16_HI:
        *(uint16_t *)P = (uint16_t)(val >> 16);
        return 0;
    case R_PPC_ADDR16_HA:
        *(uint16_t *)P = ha16(val);
        return 0;
    case R_PPC_ADDR16:
        if ((int32_t)val != (int16_t)val) goto range;
        *(uint16_t *)P = (uint16_t)val;
        return 0;
    case R_PPC_ADDR24:
        if (val & 3 || (val & 0xFC000000)) goto range;
        *insn = (*insn & 0xFC000003) | (val & 0x03FFFFFC);
        return 0;
    case R_PPC_REL24:
    case R_PPC_PLTREL24:
    case R_PPC_LOCAL24PC:
        rel = (int32_t)(val - (uint32_t)(uintptr_t)P);
        if (rel < -0x02000000 || rel >= 0x02000000) {
            /* Too far for a direct branch: go through a stub. */
            uint32_t *stub = (uint32_t *)(stubs + *stub_used);
            *stub_used += MODULE_STUB_SIZE;
            stub[0] = 0x3D800000u | (val >> 16);            /* lis   r12, hi */
            stub[1] = 0x618C0000u | (val & 0xFFFF);         /* ori   r12, r12, lo */
            stub[2] = 0x7D8903A6u;                          /* mtctr r12 */
            stub[3] = 0x4E800420u;                          /* bctr */
            rel = (int32_t)((uint32_t)(uintptr_t)stub - (uint32_t)(uintptr_t)P);
        }
        *insn = (*insn & 0xFC000003) | ((uint32_t)rel & 0x03FFFFFC);
        return 0;
    case R_PPC_REL14:
        rel = (int32_t)(val - (uint32_t)(uintptr_t)P);
        if (rel < -0x8000 || rel >= 0x8000) goto range;
        *insn = (*insn & 0xFFFF0003) | ((uint32_t)rel & 0xFFFC);
        return 0;
    case R_PPC_ADDR14:
        if ((int32_t)val != (int16_t)val) goto range;
        *insn = (*insn & 0xFFFF0003) | (val & 0xFFFC);
        return 0;
    default:
        kprintf("module: unsupported relocation type %u against %s\n", type, symname);
        return -ENOEXEC;
    }
range:
    kprintf("module: relocation against %s out of range\n", symname);
    return -ENOEXEC;
}
