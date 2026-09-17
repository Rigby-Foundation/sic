/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* AltiVec register file save/restore. The kernel itself is built
 * -mno-altivec; these are the only vector instructions in it. The buffer is
 * struct ppc_vec_state: v0..v31 (16 bytes each, 16-byte aligned), then VSCR
 * in the low word of one more vector slot, then VRSAVE.
 * The caller has MSR[VEC] set. */
#include "asm/ppc_regs.h"

    .text
    .globl ppc_vec_save
ppc_vec_save:
    li      4, 0
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    stvx    \n, 3, 4
    addi    4, 4, 16
.endr
    mfvscr  0
    stvx    0, 3, 4                     /* slot 32: VSCR (element 3); v0 is already saved */
    mfspr   5, SPRN_VRSAVE
    stw     5, 528(3)
    blr

    .globl ppc_vec_restore
ppc_vec_restore:
    lwz     5, 528(3)
    mtspr   SPRN_VRSAVE, 5
    li      4, 512
    lvx     0, 3, 4
    mtvscr  0
    li      4, 0
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    lvx     \n, 3, 4
    addi    4, 4, 16
.endr
    blr
