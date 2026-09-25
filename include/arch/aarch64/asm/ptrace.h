/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The register frame every exception, interrupt and system call saves
 * (kernel/arch/aarch64/entry.S). One layout serves them all. */
#pragma once
#include "types.h"

struct pt_regs {
    uint64_t x[31];                 /* x0..x30 (x30 = lr) */
    uint64_t sp;                    /* SP_EL0 for user frames, the interrupted sp for kernel ones */
    uint64_t pc;                    /* ELR_EL1 */
    uint64_t pstate;                /* SPSR_EL1 */
    uint64_t esr, far;              /* syndrome and fault address */
    uint64_t orig_x0;               /* first argument at entry (syscall restart) */
    uint64_t pad;                   /* 16-byte multiple: 38 words */
};

typedef struct pt_regs syscall_frame_t;
struct syscall_frame { struct pt_regs r; };
struct interrupt_frame { struct pt_regs r; };

/* AAPCS64 Linux convention: number in x8, arguments x0..x5, result in x0
 * (negative errno, what musl expects). */
#define SYSCALL_NR(f)       ((f)->r.x[8])
#define SYSCALL_ARG1(f)     ((f)->r.x[0])
#define SYSCALL_ARG2(f)     ((f)->r.x[1])
#define SYSCALL_ARG3(f)     ((f)->r.x[2])
#define SYSCALL_ARG4(f)     ((f)->r.x[3])
#define SYSCALL_ARG5(f)     ((f)->r.x[4])
#define SYSCALL_ARG6(f)     ((f)->r.x[5])
#define SYSCALL_SET_RET(f, v) ((f)->r.x[0] = (uint64_t)(long)(v))
#define FRAME_FROM_USER(f)  (((f)->r.pstate & 0xf) == 0)      /* EL0t */
#define FRAME_PC(f)         ((f)->r.pc)
#define FRAME_SP(f)         ((f)->r.sp)
