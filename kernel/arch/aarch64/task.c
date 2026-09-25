/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Task context on aarch64: kernel stacks, the SIMD/FP register file, the
 * thread pointer, address spaces. */
#include "asm/task.h"
#include "asm/ptrace.h"
#include "asm/irqflags.h"
#include "asm/sysreg.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "mm/vmm.h"
#include "string.h"
#include "printf.h"

extern void switch_context(uint64_t *old_sp, uint64_t new_sp);
extern void task_trampoline_aarch64(void);
extern void enter_frame(struct pt_regs *regs) __attribute__((noreturn));
extern void aarch64_mmu_activate(pgd_t pgd);

#define USER_PSTATE 0x0             /* EL0t, everything unmasked */

void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve)
{
    /* switch_context pops x19..x30 from 96 bytes at sp: x19 = entry,
     * x20 = arg, x30 = the trampoline. */
    uint64_t top = t->kstack_top - ((reserve + 15) & ~15UL);
    uint64_t *frame = (uint64_t *)(uintptr_t)(top - 96);
    memset(frame, 0, 96);
    frame[0] = (uint64_t)(uintptr_t)entry;                     /* x19 */
    frame[1] = (uint64_t)(uintptr_t)arg;                       /* x20 */
    frame[11] = (uint64_t)(uintptr_t)task_trampoline_aarch64;  /* x30 */
    t->arch.sp = (uint64_t)(uintptr_t)frame;
}

void arch_task_init_user(struct task *t)
{
    t->arch.tls = 0;
    memset(&t->arch.fp, 0, sizeof t->arch.fp);
}

/* The child of fork/clone resumes from a copy of the parent's frame with x0 = 0.
 * The frame comes as the argument: interrupts are on here, and a
 * task_current() preempted between its two loads (the CPU, then its
 * current task) and moved to another CPU answers with whoever runs on the
 * old one; entering *that* task's frame put two CPUs on one kernel stack. */
static void fork_thunk(void *arg)
{
    enter_frame(arg);
}

void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls)
{
    struct pt_regs *cf = (struct pt_regs *)(uintptr_t)(child->kstack_top - sizeof(struct pt_regs));
    arch_task_setup(child, fork_thunk, cf, sizeof(struct pt_regs));
    *cf = f->r;
    cf->x[0] = 0;
    if (stack) cf->sp = stack;
    child->arch.tls = tls ? tls : (uint64_t)read_sysreg(tpidr_el0);
    /* The parent is running: its SIMD/FP registers are live in the hardware. */
    fpsimd_save(&parent->arch.fp);
    child->arch.fp = parent->arch.fp;
}

uint64_t arch_frame_user_sp(const struct syscall_frame *f) { return f->r.sp; }

void arch_switch(struct task *prev, struct task *next)
{
    if (prev->is_user) { fpsimd_save(&prev->arch.fp); prev->arch.tls = read_sysreg(tpidr_el0); }
    if (next->is_user) { fpsimd_restore(&next->arch.fp); write_sysreg(tpidr_el0, next->arch.tls); }
    cpu_set_kernel_stack(next->kstack_top);
    if (next->pgd != prev->pgd)
        aarch64_mmu_activate(next->pgd);
    switch_context(&prev->arch.sp, next->arch.sp);
}

void arch_switch_first(struct task *next)
{
    uint64_t scratch;
    if (next->is_user) { fpsimd_restore(&next->arch.fp); write_sysreg(tpidr_el0, next->arch.tls); }
    aarch64_mmu_activate(next->pgd);
    switch_context(&scratch, next->arch.sp);
    __builtin_unreachable();
}

void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp)
{
    aarch64_mmu_activate(t->pgd);
    struct pt_regs *r = (struct pt_regs *)(uintptr_t)(t->kstack_top - sizeof(struct pt_regs));
    memset(r, 0, sizeof *r);
    r->pc = entry;
    r->pstate = USER_PSTATE;
    r->sp = user_sp;
    write_sysreg(tpidr_el0, t->arch.tls);
    memset(&t->arch.fp, 0, sizeof t->arch.fp);
    fpsimd_restore(&t->arch.fp);
    interrupts_disable();
    enter_frame(r);
}
