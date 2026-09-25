/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* aarch64 side of the module loader: ELF64 RELA relocations of the small
 * code model. Module memory is direct-map pages, so ADRP reaches the
 * kernel (both are within 4 GiB of each other) but a `bl` may not: those
 * get a stub (ldr x16, target; br x16) in the module. */
#include "module_arch.h"
#include "mm/pmm.h"
#include "asm/memlayout.h"
#include "abi/abi.h"
#include "printf.h"

#define R_AARCH64_NONE            0
#define R_AARCH64_ABS64           257
#define R_AARCH64_ABS32           258
#define R_AARCH64_ABS16           259
#define R_AARCH64_PREL64          260
#define R_AARCH64_PREL32          261
#define R_AARCH64_PREL16          262
#define R_AARCH64_MOVW_UABS_G0_NC 264
#define R_AARCH64_ADR_PREL_LO21   274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC  278
#define R_AARCH64_TSTBR14         279
#define R_AARCH64_CONDBR19        280
#define R_AARCH64_JUMP26          282
#define R_AARCH64_CALL26          283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299

void *arch_module_alloc(size_t pages)
{
    uint64_t base = pmm_alloc_pages(pages);
    return base ? P2V(base) : NULL;
}

void arch_module_free(void *base, size_t pages) { pmm_free_pages(V2P(base), pages); }

void arch_module_flush(void *base, size_t size)
{
    uintptr_t a = (uintptr_t)base & ~63UL, e = (uintptr_t)base + size;
    for (uintptr_t p = a; p < e; p += 64) __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish");
    for (uintptr_t p = a; p < e; p += 64) __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish; isb");
}

static inline uint32_t get32(const uint8_t *p) { return *(const uint32_t *)p; }
static inline void put32(uint8_t *p, uint32_t v) { *(uint32_t *)p = v; }

/* imm26 branches: |disp| < 128 MiB */
static int in_branch_range(int64_t disp) { return disp >= -(1LL << 27) && disp < (1LL << 27); }

int arch_module_reloc(uint8_t *P, uint32_t type, unsigned long S, long A,
                      uint8_t *stubs, size_t *stub_used, const char *symname)
{
    uint64_t val = (uint64_t)S + (uint64_t)A;
    int64_t rel = (int64_t)(val - (uint64_t)(uintptr_t)P);
    uint32_t insn = get32(P);

    switch (type) {
    case R_AARCH64_NONE: return 0;
    case R_AARCH64_ABS64: *(uint64_t *)P = val; return 0;
    case R_AARCH64_ABS32: *(uint32_t *)P = (uint32_t)val; return 0;
    case R_AARCH64_ABS16: *(uint16_t *)P = (uint16_t)val; return 0;
    case R_AARCH64_PREL64: *(uint64_t *)P = (uint64_t)rel; return 0;
    case R_AARCH64_PREL32: *(uint32_t *)P = (uint32_t)rel; return 0;
    case R_AARCH64_PREL16: *(uint16_t *)P = (uint16_t)rel; return 0;
    case R_AARCH64_MOVW_UABS_G0_NC:
        put32(P, (insn & ~(0xffffu << 5)) | ((uint32_t)(val & 0xffff) << 5)); return 0;
    case R_AARCH64_ADR_PREL_LO21:
        if (rel < -(1 << 20) || rel >= (1 << 20)) goto range;
        put32(P, (insn & 0x9f00001f) | ((uint32_t)(rel & 3) << 29) | ((uint32_t)((rel >> 2) & 0x7ffff) << 5));
        return 0;
    case R_AARCH64_ADR_PREL_PG_HI21: {
        int64_t pg = (int64_t)((val & ~0xfffULL) - ((uint64_t)(uintptr_t)P & ~0xfffULL)) >> 12;
        if (pg < -(1 << 20) || pg >= (1 << 20)) goto range;
        put32(P, (insn & 0x9f00001f) | ((uint32_t)(pg & 3) << 29) | ((uint32_t)((pg >> 2) & 0x7ffff) << 5));
        return 0;
    }
    case R_AARCH64_ADD_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)(val & 0xfff) << 10)); return 0;
    case R_AARCH64_LDST8_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)(val & 0xfff) << 10)); return 0;
    case R_AARCH64_LDST16_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)((val & 0xfff) >> 1) << 10)); return 0;
    case R_AARCH64_LDST32_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)((val & 0xfff) >> 2) << 10)); return 0;
    case R_AARCH64_LDST64_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)((val & 0xfff) >> 3) << 10)); return 0;
    case R_AARCH64_LDST128_ABS_LO12_NC:
        put32(P, (insn & ~(0xfffu << 10)) | ((uint32_t)((val & 0xfff) >> 4) << 10)); return 0;
    case R_AARCH64_TSTBR14:
        if (rel < -(1 << 15) || rel >= (1 << 15)) goto range;
        put32(P, (insn & ~(0x3fffu << 5)) | ((uint32_t)((rel >> 2) & 0x3fff) << 5)); return 0;
    case R_AARCH64_CONDBR19:
        if (rel < -(1 << 20) || rel >= (1 << 20)) goto range;
        put32(P, (insn & ~(0x7ffffu << 5)) | ((uint32_t)((rel >> 2) & 0x7ffff) << 5)); return 0;
    case R_AARCH64_JUMP26:
    case R_AARCH64_CALL26:
        if (!in_branch_range(rel)) {
            /* A stub: ldr x16, [pc, #8]; br x16; .quad target */
            uint8_t *st = stubs + *stub_used;
            put32(st, 0x58000050);          /* ldr x16, .+8 */
            put32(st + 4, 0xd61f0200);      /* br x16 */
            *(uint64_t *)(st + 8) = val;
            *stub_used += MODULE_STUB_SIZE;
            rel = (int64_t)((uint64_t)(uintptr_t)st - (uint64_t)(uintptr_t)P);
            if (!in_branch_range(rel)) goto range;
        }
        put32(P, (insn & 0xfc000000) | ((uint32_t)(rel >> 2) & 0x03ffffff));
        return 0;
    default:
        kprintf("module: unsupported relocation type %u (%s)\n", type, symname);
        return -ENOEXEC;
    }
range:
    kprintf("module: relocation %u out of range for %s\n", type, symname);
    return -ENOEXEC;
}
