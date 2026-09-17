/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The register frame every exception, interrupt and system call saves
 * (kernel/arch/powerpc/entry.S). One layout serves them all. */
#pragma once
#include "types.h"

struct pt_regs {
    uint32_t gpr[32];
    uint32_t nip, msr, ctr, link, xer, ccr;
    uint32_t dar, dsisr;            /* fault address / status for DSI, ISI */
    uint32_t trap;                  /* exception vector */
    uint32_t orig_r3;               /* first argument at entry (syscall restart) */
    uint32_t pad[2];                /* 16-byte alignment: 44 words */
};

typedef struct pt_regs syscall_frame_t;
struct syscall_frame { struct pt_regs r; };
struct interrupt_frame { struct pt_regs r; };

/* System V PowerPC: number in r0, arguments r3..r8, result in r3. Errors
 * follow the Linux convention musl's wrappers expect (sc; bns+; neg): a
 * positive errno in r3 with CR0.SO set, SO clear on success. The generic code
 * hands us -errno; the macro converts, so signal frames and fork's child see
 * the user-visible values. */
#define SYSCALL_NR(f)       ((f)->r.gpr[0])
#define SYSCALL_ARG1(f)     ((f)->r.gpr[3])
#define SYSCALL_ARG2(f)     ((f)->r.gpr[4])
#define SYSCALL_ARG3(f)     ((f)->r.gpr[5])
#define SYSCALL_ARG4(f)     ((f)->r.gpr[6])
#define SYSCALL_ARG5(f)     ((f)->r.gpr[7])
#define SYSCALL_ARG6(f)     ((f)->r.gpr[8])
#define SYSCALL_SET_RET(f, v) do { \
        long _v = (long)(v); \
        if (_v < 0 && _v > -4096) { (f)->r.gpr[3] = (uint32_t)-_v; (f)->r.ccr |= 0x10000000u; } \
        else { (f)->r.gpr[3] = (uint32_t)_v; (f)->r.ccr &= ~0x10000000u; } \
    } while (0)
#define FRAME_FROM_USER(f)  (((f)->r.msr & (1UL << 14)) != 0)     /* MSR_PR */
#define FRAME_PC(f)         ((f)->r.nip)
#define FRAME_SP(f)         ((f)->r.gpr[1])
