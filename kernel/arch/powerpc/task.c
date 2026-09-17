/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Task context on 32-bit PowerPC: kernel stacks, FPU and AltiVec state,
 * TLS (r2), address spaces. */
#include "asm/task.h"
#include "asm/ptrace.h"
#include "asm/irqflags.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "mm/vmm.h"
#include "string.h"

extern void switch_context(uint32_t *old_sp, uint32_t new_sp);
extern void task_trampoline_ppc(void);
extern void enter_frame(struct pt_regs *regs) __attribute__((noreturn));
extern void ppc_mmu_activate(pgd_t pgd);
extern void ppc_vec_save(struct ppc_vec_state *v);
extern void ppc_vec_restore(struct ppc_vec_state *v);

#define KERNEL_MSR (MSR_ME | MSR_IR | MSR_DR | MSR_RI)
#define USER_MSR   (KERNEL_MSR | MSR_EE | MSR_PR)

void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve)
{
    /* switch_context pops: CR at 8(sp), r14..r31 at 12(sp), LR from 100(sp);
     * we make LR = task_trampoline_ppc, r14 = entry, r15 = arg. */
    uint32_t top = (uint32_t)(t->kstack_top - ((reserve + 15) & ~15UL));
    uint32_t *frame = (uint32_t *)(uintptr_t)(top - 112);       /* 96 for switch_context + 16 caller frame */
    memset(frame, 0, 112);
    frame[2] = 0;                                   /* CR */
    frame[3] = (uint32_t)(uintptr_t)entry;          /* r14 */
    frame[4] = (uint32_t)(uintptr_t)arg;            /* r15 */
    frame[25] = (uint32_t)(uintptr_t)task_trampoline_ppc;   /* 100(sp): LR save slot in the frame above */
    frame[24] = 0;                                  /* back chain of the frame above */
    t->arch.sp = (uint32_t)(uintptr_t)frame;
    t->arch.fp_used = 0;
    t->arch.vec_used = 0;
}

void arch_task_init_user(struct task *t)
{
    t->arch.tls = 0;
    t->arch.fp_used = 0;
    memset(t->arch.fpr, 0, sizeof(t->arch.fpr));
    t->arch.fpscr = 0;
    t->arch.vec_used = 0;
    memset(&t->arch.vec, 0, sizeof(t->arch.vec));
}

/* The child of fork/clone resumes from a copy of the parent's frame with r3 = 0. */
static void fork_thunk(void *arg)
{
    (void)arg;
    struct pt_regs *r = (struct pt_regs *)(uintptr_t)(task_current()->kstack_top - sizeof(struct pt_regs) - 16);
    enter_frame(r);
}

void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls)
{
    arch_task_setup(child, fork_thunk, NULL, sizeof(struct pt_regs) + 16);
    struct pt_regs *cf = (struct pt_regs *)(uintptr_t)(child->kstack_top - sizeof(struct pt_regs) - 16);
    *cf = f->r;
    cf->gpr[3] = 0;
    cf->ccr &= ~0x10000000u;                 /* success: SO clear */
    if (stack) cf->gpr[1] = (uint32_t)stack;
    if (tls) cf->gpr[2] = (uint32_t)tls;
    child->arch.tls = tls ? (uint32_t)tls : parent->arch.tls;
    /* The parent is running: its live FP/vector registers are in the
     * hardware, not yet in arch. Pull them so the child starts with them. */
    if (parent->arch.fp_used && (f->r.msr & MSR_FP))
        ppc_fpu_save_for_signal(parent, f->r.msr);
    if (parent->arch.vec_used && (f->r.msr & MSR_VEC))
        ppc_vec_save_for_signal(parent, f->r.msr);
    child->arch.fp_used = parent->arch.fp_used;
    memcpy(child->arch.fpr, parent->arch.fpr, sizeof(child->arch.fpr));
    child->arch.fpscr = parent->arch.fpscr;
    child->arch.vec_used = parent->arch.vec_used;
    child->arch.vec = parent->arch.vec;
    /* The child reloads lazily through the unavailable traps. */
    cf->msr &= ~(MSR_FP | MSR_VEC);
}

uint64_t arch_frame_user_sp(const struct syscall_frame *f) { return f->r.gpr[1]; }

/* ---- FPU: lazily enabled per task, saved/restored around switches ------------- */

static void fpu_save(struct arch_task *a)
{
    unsigned long m = mfmsr();
    mtmsr(m | MSR_FP);
    __asm__ volatile(
        "stfd 0,0(%0); stfd 1,8(%0); stfd 2,16(%0); stfd 3,24(%0); stfd 4,32(%0); stfd 5,40(%0); stfd 6,48(%0); stfd 7,56(%0);"
        "stfd 8,64(%0); stfd 9,72(%0); stfd 10,80(%0); stfd 11,88(%0); stfd 12,96(%0); stfd 13,104(%0); stfd 14,112(%0); stfd 15,120(%0);"
        "stfd 16,128(%0); stfd 17,136(%0); stfd 18,144(%0); stfd 19,152(%0); stfd 20,160(%0); stfd 21,168(%0); stfd 22,176(%0); stfd 23,184(%0);"
        "stfd 24,192(%0); stfd 25,200(%0); stfd 26,208(%0); stfd 27,216(%0); stfd 28,224(%0); stfd 29,232(%0); stfd 30,240(%0); stfd 31,248(%0);"
        "mffs 0; stfd 0,256(%0)" : : "r"(a->fpr) : "memory");
    mtmsr(m);
}

static void fpu_restore(struct arch_task *a)
{
    unsigned long m = mfmsr();
    mtmsr(m | MSR_FP);
    __asm__ volatile(
        "lfd 0,256(%0); mtfsf 0xff,0;"
        "lfd 0,0(%0); lfd 1,8(%0); lfd 2,16(%0); lfd 3,24(%0); lfd 4,32(%0); lfd 5,40(%0); lfd 6,48(%0); lfd 7,56(%0);"
        "lfd 8,64(%0); lfd 9,72(%0); lfd 10,80(%0); lfd 11,88(%0); lfd 12,96(%0); lfd 13,104(%0); lfd 14,112(%0); lfd 15,120(%0);"
        "lfd 16,128(%0); lfd 17,136(%0); lfd 18,144(%0); lfd 19,152(%0); lfd 20,160(%0); lfd 21,168(%0); lfd 22,176(%0); lfd 23,184(%0);"
        "lfd 24,192(%0); lfd 25,200(%0); lfd 26,208(%0); lfd 27,216(%0); lfd 28,224(%0); lfd 29,232(%0); lfd 30,240(%0); lfd 31,248(%0)"
        : : "r"(a->fpr) : "memory");
    mtmsr(m);
}

/* FP unavailable trap: enable it in the task's return MSR and load its state. */
void ppc_fpu_enable_for(struct task *t, struct pt_regs *r)
{
    if (!t->arch.fp_used) {
        t->arch.fp_used = 1;
        memset(t->arch.fpr, 0, sizeof(t->arch.fpr));
        t->arch.fpscr = 0;
    }
    fpu_restore(&t->arch);
    r->msr |= MSR_FP;
}

/* ---- AltiVec: the same scheme, 512 bytes bigger ------------------------------ */

static void vec_save(struct arch_task *a)
{
    unsigned long m = mfmsr();
    mtmsr(m | MSR_VEC);
    __asm__ volatile("isync");
    ppc_vec_save(&a->vec);
    mtmsr(m);
}

static void vec_restore(struct arch_task *a)
{
    unsigned long m = mfmsr();
    mtmsr(m | MSR_VEC);
    __asm__ volatile("isync");
    ppc_vec_restore(&a->vec);
    mtmsr(m);
}

/* AltiVec unavailable trap: enable it in the task's return MSR and load its state. */
void ppc_vec_enable_for(struct task *t, struct pt_regs *r)
{
    if (!t->arch.vec_used) {
        t->arch.vec_used = 1;
        memset(&t->arch.vec, 0, sizeof(t->arch.vec));
    }
    vec_restore(&t->arch);
    r->msr |= MSR_VEC;
}

/* Signal delivery interrupts a task in the middle of using the FPU/vector
 * unit: capture the live registers into arch so the frame can keep them and
 * the handler can start from the same values. `msr` is the interrupted MSR. */
void ppc_fpu_save_for_signal(struct task *t, unsigned long msr)
{
    if (t->arch.fp_used && (msr & MSR_FP))
        fpu_save(&t->arch);
}

void ppc_vec_save_for_signal(struct task *t, unsigned long msr)
{
    if (t->arch.vec_used && (msr & MSR_VEC))
        vec_save(&t->arch);
}

void arch_switch(struct task *prev, struct task *next)
{
    if (prev->is_user && prev->arch.fp_used)
        fpu_save(&prev->arch);
    if (next->is_user && next->arch.fp_used)
        fpu_restore(&next->arch);
    if (prev->is_user && prev->arch.vec_used)
        vec_save(&prev->arch);
    if (next->is_user && next->arch.vec_used)
        vec_restore(&next->arch);
    cpu_set_kernel_stack(next->kstack_top);
    if (next->pgd != prev->pgd)
        ppc_mmu_activate(next->pgd);
    switch_context(&prev->arch.sp, next->arch.sp);
}

void arch_switch_first(struct task *next)
{
    uint32_t scratch;
    switch_context(&scratch, next->arch.sp);
    __builtin_unreachable();
}

void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp)
{
    ppc_mmu_activate(t->pgd);
    struct pt_regs r;
    memset(&r, 0, sizeof r);
    r.nip = (uint32_t)entry;
    r.msr = USER_MSR;
    r.gpr[1] = (uint32_t)user_sp;
    r.gpr[2] = t->arch.tls;
    interrupts_disable();
    enter_frame(&r);
}
