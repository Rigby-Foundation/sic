/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Exception vectors, the common entry/exit path, context switch and the
 * drop into user mode.
 *
 * The vectors are assembled here and copied to physical 0 by
 * ppc_install_vectors(); everything inside .vectors is position-independent
 * (relative branches only) until it has switched the MMU back on and jumped
 * to ppc_trap by absolute address.
 *
 * An exception arrives in real mode (MSR IR/DR off). Kernel stacks live in
 * the direct map, so their physical address is virtual - 0xC0000000, which
 * is how the prolog can save registers before turning translation back on.
 */
#include "asm/ppc_regs.h"

/* struct pt_regs offsets */
#define GPR(n)      (4 * (n))
#define PT_NIP      128
#define PT_MSR      132
#define PT_CTR      136
#define PT_LINK     140
#define PT_XER      144
#define PT_CCR      148
#define PT_DAR      152
#define PT_DSISR    156
#define PT_TRAP     160
#define PT_ORIG_R3  164
#define PT_SIZE     176
#define FRAME_SIZE  (PT_SIZE + 16)      /* + back chain / LR save area below */

#define CPU_KERNEL_SP 4

#define KERNEL_VIRT_HI 0xC000

    .section .vectors, "ax"
    .globl ppc_vectors_start, ppc_vectors_end
ppc_vectors_start:

.macro VECTOR num
    .org ppc_vectors_start + \num
    mtspr   SPRN_SPRG0, 10
    mtspr   SPRN_SPRG1, 11
    mtspr   SPRN_SPRG2, 12
    li      12, \num
    b       exception_common
.endm

    /* 0x000 is not a vector; the first is system reset at 0x100. */
    .org ppc_vectors_start
    b       .

    VECTOR 0x100
    VECTOR 0x200
    VECTOR 0x300
    VECTOR 0x400
    VECTOR 0x500
    VECTOR 0x600
    VECTOR 0x700
    VECTOR 0x800
    VECTOR 0x900
    VECTOR 0xA00
    VECTOR 0xB00
    VECTOR 0xC00
    VECTOR 0xD00
    VECTOR 0xE00
    VECTOR 0xF00
    .org ppc_vectors_start + 0xF20
    mtspr   SPRN_SPRG0, 10
    mtspr   SPRN_SPRG1, 11
    mtspr   SPRN_SPRG2, 12
    li      12, 0xF20
    b       exception_common
    VECTOR 0x1000
    VECTOR 0x1100
    VECTOR 0x1200
    VECTOR 0x1300
    VECTOR 0x1400
    VECTOR 0x1700

    .org ppc_vectors_start + 0x2000
exception_common:
    /* r10, r11, r12 are free (saved in SPRG0-2); r12 = vector. */
    mfcr    10                          /* CR into r10 for the frame */
    mfspr   11, SPRN_SRR1
    andi.   11, 11, (1 << MSR_PR_BIT)
    beq     1f
    /* From user mode: the current task's kernel stack (per-CPU block via SPRG3). */
    mfspr   11, SPRN_SPRG3
    addis   11, 11, -KERNEL_VIRT_HI     /* to physical */
    lwz     11, CPU_KERNEL_SP(11)
    b       2f
1:  mr      11, 1                       /* from kernel mode: stay on this stack */
2:  addis   11, 11, -KERNEL_VIRT_HI     /* physical */
    subi    11, 11, FRAME_SIZE
    stw     1, GPR(1)+16(11)
    stw     10, PT_CCR+16(11)
    stw     0, GPR(0)+16(11)
    stw     2, GPR(2)+16(11)
    stw     3, GPR(3)+16(11)
    stw     3, PT_ORIG_R3+16(11)
    stw     4, GPR(4)+16(11)
    stw     5, GPR(5)+16(11)
    stw     6, GPR(6)+16(11)
    stw     7, GPR(7)+16(11)
    stw     8, GPR(8)+16(11)
    stw     9, GPR(9)+16(11)
    mfspr   10, SPRN_SPRG0
    stw     10, GPR(10)+16(11)
    mfspr   10, SPRN_SPRG1
    stw     10, GPR(11)+16(11)
    mfspr   10, SPRN_SPRG2
    stw     10, GPR(12)+16(11)
    stw     12, PT_TRAP+16(11)
    stw     13, GPR(13)+16(11)
    stw     14, GPR(14)+16(11)
    stw     15, GPR(15)+16(11)
    stw     16, GPR(16)+16(11)
    stw     17, GPR(17)+16(11)
    stw     18, GPR(18)+16(11)
    stw     19, GPR(19)+16(11)
    stw     20, GPR(20)+16(11)
    stw     21, GPR(21)+16(11)
    stw     22, GPR(22)+16(11)
    stw     23, GPR(23)+16(11)
    stw     24, GPR(24)+16(11)
    stw     25, GPR(25)+16(11)
    stw     26, GPR(26)+16(11)
    stw     27, GPR(27)+16(11)
    stw     28, GPR(28)+16(11)
    stw     29, GPR(29)+16(11)
    stw     30, GPR(30)+16(11)
    stw     31, GPR(31)+16(11)
    mflr    10
    stw     10, PT_LINK+16(11)
    mfctr   10
    stw     10, PT_CTR+16(11)
    mfxer   10
    stw     10, PT_XER+16(11)
    mfspr   10, SPRN_SRR0
    stw     10, PT_NIP+16(11)
    mfspr   10, SPRN_SRR1
    stw     10, PT_MSR+16(11)
    mfspr   10, SPRN_DAR
    stw     10, PT_DAR+16(11)
    mfspr   10, SPRN_DSISR
    stw     10, PT_DSISR+16(11)
    li      10, 0
    stw     10, 0(11)                   /* back chain: this frame ends the kernel stack walk */

    /* Switch translation on and continue at ppc_trap(regs) in the direct map. */
    addis   1, 11, KERNEL_VIRT_HI       /* virtual stack pointer */
    addi    3, 1, 16                    /* regs */
    lis     10, ppc_trap@h
    ori     10, 10, ppc_trap@l
    mtspr   SPRN_SRR0, 10
    mfmsr   10
    ori     10, 10, (1 << MSR_IR_BIT) | (1 << MSR_DR_BIT)
    rlwinm  10, 10, 0, 17, 15           /* EE off */
    mtspr   SPRN_SRR1, 10
    lis     10, ret_from_exception@h
    ori     10, 10, ret_from_exception@l
    mtlr    10
    rfi
ppc_vectors_end:

    .text

/* ppc_trap returned: restore everything from the frame at r1+16 and rfi. */
    .globl ret_from_exception
ret_from_exception:
    mfmsr   10
    rlwinm  10, 10, 0, 17, 15           /* EE off while SRR0/SRR1 are live */
    mtmsr   10
    isync
    addi    11, 1, 16
    lwz     10, PT_NIP(11)
    mtspr   SPRN_SRR0, 10
    lwz     10, PT_MSR(11)
    mtspr   SPRN_SRR1, 10
    lwz     10, PT_LINK(11)
    mtlr    10
    lwz     10, PT_CTR(11)
    mtctr   10
    lwz     10, PT_XER(11)
    mtxer   10
    lwz     10, PT_CCR(11)
    mtcr    10
    lwz     0, GPR(0)(11)
    lwz     2, GPR(2)(11)
    lwz     3, GPR(3)(11)
    lwz     4, GPR(4)(11)
    lwz     5, GPR(5)(11)
    lwz     6, GPR(6)(11)
    lwz     7, GPR(7)(11)
    lwz     8, GPR(8)(11)
    lwz     9, GPR(9)(11)
    lwz     10, GPR(10)(11)
    lwz     12, GPR(12)(11)
    lwz     13, GPR(13)(11)
    lwz     14, GPR(14)(11)
    lwz     15, GPR(15)(11)
    lwz     16, GPR(16)(11)
    lwz     17, GPR(17)(11)
    lwz     18, GPR(18)(11)
    lwz     19, GPR(19)(11)
    lwz     20, GPR(20)(11)
    lwz     21, GPR(21)(11)
    lwz     22, GPR(22)(11)
    lwz     23, GPR(23)(11)
    lwz     24, GPR(24)(11)
    lwz     25, GPR(25)(11)
    lwz     26, GPR(26)(11)
    lwz     27, GPR(27)(11)
    lwz     28, GPR(28)(11)
    lwz     29, GPR(29)(11)
    lwz     30, GPR(30)(11)
    lwz     31, GPR(31)(11)
    lwz     1, GPR(1)(11)
    lwz     11, GPR(11)(11)
    rfi

/* void enter_frame(struct pt_regs *regs): become the context described by
 * regs (exec, the first user entry, fork's child). Never returns. */
    .globl enter_frame
enter_frame:
    addi    1, 3, -16
    b       ret_from_exception

/* void switch_context(uint32_t *old_sp, uint32_t new_sp)
 * Non-volatile state on the stack: r14-r31, CR, LR. */
    .globl switch_context
switch_context:
    stwu    1, -96(1)
    mflr    0
    stw     0, 100(1)                   /* LR in the caller's frame slot */
    mfcr    0
    stw     0, 8(1)
    stmw    14, 12(1)                   /* r14..r31: 18 words */
    stw     1, 0(3)
    mr      1, 4
    lmw     14, 12(1)
    lwz     0, 8(1)
    mtcr    0
    lwz     0, 100(1)
    mtlr    0
    addi    1, 1, 96
    blr

/* First run of a new task: switch_context "returns" here with r14 = entry,
 * r15 = arg (arch_task_setup put them on the stack). */
    .globl task_trampoline_ppc
task_trampoline_ppc:
    bl      sched_unlock_new_task
    mfmsr   3
    ori     3, 3, (1 << MSR_EE_BIT)     /* interrupts on, like the sti in the x86 trampoline */
    mtmsr   3
    isync
    mr      3, 15
    mtctr   14
    bctrl
    bl      task_exit
1:  b       1b
