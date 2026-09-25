/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Exception vectors, the common entry/exit path, context switch, the drop
 * into user mode, and the SIMD/FP register file save/restore.
 *
 * The kernel runs on SP_EL1, which is the current task's kernel stack:
 * on the way out to EL0 the frame is popped from the top of it, so
 * SP_EL1 is left at the stack top for the next exception. */

/* struct pt_regs offsets */
#define PT_X(n)     (8 * (n))
#define PT_SP       248
#define PT_PC       256
#define PT_PSTATE   264
#define PT_ESR      272
#define PT_FAR      280
#define PT_ORIG_X0  288
#define PT_SIZE     304

/* Save everything into a fresh frame on SP_EL1. `el` picks where the
 * interrupted sp came from. */
.macro SAVE_FRAME el
    sub     sp, sp, #PT_SIZE
    stp     x0, x1, [sp, #PT_X(0)]
    stp     x2, x3, [sp, #PT_X(2)]
    stp     x4, x5, [sp, #PT_X(4)]
    stp     x6, x7, [sp, #PT_X(6)]
    stp     x8, x9, [sp, #PT_X(8)]
    stp     x10, x11, [sp, #PT_X(10)]
    stp     x12, x13, [sp, #PT_X(12)]
    stp     x14, x15, [sp, #PT_X(14)]
    stp     x16, x17, [sp, #PT_X(16)]
    stp     x18, x19, [sp, #PT_X(18)]
    stp     x20, x21, [sp, #PT_X(20)]
    stp     x22, x23, [sp, #PT_X(22)]
    stp     x24, x25, [sp, #PT_X(24)]
    stp     x26, x27, [sp, #PT_X(26)]
    stp     x28, x29, [sp, #PT_X(28)]
.if \el == 0
    mrs     x21, sp_el0
.else
    add     x21, sp, #PT_SIZE
.endif
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    mrs     x24, esr_el1
    mrs     x25, far_el1
    stp     x30, x21, [sp, #PT_X(30)]
    stp     x22, x23, [sp, #PT_PC]
    stp     x24, x25, [sp, #PT_ESR]
    str     x0, [sp, #PT_ORIG_X0]
    mov     x29, xzr                    /* the frame ends the kernel stack walk */
.endm

.macro VENTRY label
    .balign 128
    b       \label
.endm

    .section .vectors, "ax"
    .balign 2048
    .globl vectors
vectors:
    /* Current EL with SP0: unused (we always run on SP_EL1) */
    VENTRY  el1_bad
    VENTRY  el1_bad
    VENTRY  el1_bad
    VENTRY  el1_bad
    /* Current EL with SPx: the kernel */
    VENTRY  el1_sync
    VENTRY  el1_irq
    VENTRY  el1_bad
    VENTRY  el1_serror
    /* Lower EL, AArch64: user programs */
    VENTRY  el0_sync
    VENTRY  el0_irq
    VENTRY  el0_bad
    VENTRY  el0_serror
    /* Lower EL, AArch32: not supported */
    VENTRY  el0_bad
    VENTRY  el0_bad
    VENTRY  el0_bad
    VENTRY  el0_bad

    .text

el1_sync:
    SAVE_FRAME 1
    mov     x0, sp
    mov     x1, #0
    bl      aarch64_sync
    b       ret_from_exception
el1_irq:
    SAVE_FRAME 1
    mov     x0, sp
    bl      aarch64_irq
    b       ret_from_exception
el1_serror:
el1_bad:
    SAVE_FRAME 1
    mov     x0, sp
    bl      aarch64_bad
    b       ret_from_exception

el0_sync:
    SAVE_FRAME 0
    mov     x0, sp
    mov     x1, #1
    bl      aarch64_sync
    b       ret_from_exception
el0_irq:
    SAVE_FRAME 0
    mov     x0, sp
    bl      aarch64_irq
    b       ret_from_exception
el0_serror:
el0_bad:
    SAVE_FRAME 0
    mov     x0, sp
    bl      aarch64_bad
    b       ret_from_exception

/* The C side returned: restore the frame at sp and eret. Interrupts go off
 * while ELR/SPSR are live. */
    .globl ret_from_exception
ret_from_exception:
    msr     daifset, #2
    ldp     x22, x23, [sp, #PT_PC]
    msr     elr_el1, x22
    msr     spsr_el1, x23
    ldp     x30, x21, [sp, #PT_X(30)]
    and     x23, x23, #0xf
    cbnz    x23, 1f
    msr     sp_el0, x21                 /* a user frame: its stack pointer */
1:  ldp     x0, x1, [sp, #PT_X(0)]
    ldp     x2, x3, [sp, #PT_X(2)]
    ldp     x4, x5, [sp, #PT_X(4)]
    ldp     x6, x7, [sp, #PT_X(6)]
    ldp     x8, x9, [sp, #PT_X(8)]
    ldp     x10, x11, [sp, #PT_X(10)]
    ldp     x12, x13, [sp, #PT_X(12)]
    ldp     x14, x15, [sp, #PT_X(14)]
    ldp     x16, x17, [sp, #PT_X(16)]
    ldp     x18, x19, [sp, #PT_X(18)]
    ldp     x20, x21, [sp, #PT_X(20)]
    ldp     x22, x23, [sp, #PT_X(22)]
    ldp     x24, x25, [sp, #PT_X(24)]
    ldp     x26, x27, [sp, #PT_X(26)]
    ldp     x28, x29, [sp, #PT_X(28)]
    add     sp, sp, #PT_SIZE
    eret

/* void enter_frame(struct pt_regs *regs): become the (user) context
 * described by regs, which sits at the top of the current kernel stack. */
    .globl enter_frame
enter_frame:
    mov     sp, x0
    b       ret_from_exception

/* void switch_context(uint64_t *old_sp, uint64_t new_sp)
 * Callee-saved state on the stack: x19-x29, x30. */
    .globl switch_context
switch_context:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x9, sp
    str     x9, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

/* First run of a new task: switch_context "returns" here with
 * x19 = entry, x20 = arg (arch_task_setup put them on the stack). */
    .globl task_trampoline_aarch64
task_trampoline_aarch64:
    bl      sched_unlock_new_task
    msr     daifclr, #2
    mov     x0, x20
    blr     x19
    bl      task_exit
1:  b       1b

/* The SIMD/FP register file: q0-q31 then fpsr, fpcr. */
    .globl fpsimd_save
fpsimd_save:
    stp     q0, q1, [x0, #0]
    stp     q2, q3, [x0, #32]
    stp     q4, q5, [x0, #64]
    stp     q6, q7, [x0, #96]
    stp     q8, q9, [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     w1, [x0, #512]
    str     w2, [x0, #516]
    ret

    .globl fpsimd_restore
fpsimd_restore:
    ldr     w1, [x0, #512]
    ldr     w2, [x0, #516]
    msr     fpsr, x1
    msr     fpcr, x2
    ldp     q0, q1, [x0, #0]
    ldp     q2, q3, [x0, #32]
    ldp     q4, q5, [x0, #64]
    ldp     q6, q7, [x0, #96]
    ldp     q8, q9, [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    ret
