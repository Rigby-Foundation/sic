/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Task context on x86_64: kernel stacks, FPU state, TLS, address spaces. */
#include "asm/task.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "mm/vmm.h"
#include "string.h"

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void task_trampoline(void);
extern void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv) __attribute__((noreturn));
extern void fork_return(struct syscall_frame *f);

void arch_task_setup(struct task *t, task_entry_t entry, void *arg, size_t reserve)
{
    memcpy(t->arch.fpu, fpu_initial_state, sizeof(t->arch.fpu));
    /* Initial frame, as switch_context expects to pop it:
     * r15 r14 r13(entry) r12(arg) rbx rbp | return -> task_trampoline. */
    uint64_t *sp = (uint64_t *)(t->kstack_top - ((reserve + 15) & ~15UL));
    *--sp = (uint64_t)task_trampoline;
    *--sp = 0;                      /* rbp */
    *--sp = 0;                      /* rbx */
    *--sp = (uint64_t)(uintptr_t)arg;   /* r12 */
    *--sp = (uint64_t)(uintptr_t)entry; /* r13 */
    *--sp = 0;                      /* r14 */
    *--sp = 0;                      /* r15 */
    t->arch.rsp = (uint64_t)(uintptr_t)sp;
}

void arch_task_init_user(struct task *t)
{
    t->arch.fs_base = 0;
    memcpy(t->arch.fpu, fpu_initial_state, sizeof(t->arch.fpu));
}

/* The child resumes right after the syscall instruction with rax = 0. */
static void fork_thunk(void *arg)
{
    fork_return(arg);                   /* the frame, not task_current(): see aarch64/task.c */
}

void arch_task_fork(struct task *child, struct task *parent, const struct syscall_frame *f, uint64_t stack, uint64_t tls)
{
    struct syscall_frame *cf = (struct syscall_frame *)(child->kstack_top - sizeof(*cf));
    arch_task_setup(child, fork_thunk, cf, sizeof(struct syscall_frame));
    *cf = *f;
    cf->rax = 0;
    if (stack)
        cf->rsp = stack;
    child->arch.fs_base = tls ? tls : parent->arch.fs_base;
    fpu_save(parent->arch.fpu);         /* the parent's live state is in the registers */
    memcpy(child->arch.fpu, parent->arch.fpu, sizeof(child->arch.fpu));
}

uint64_t arch_frame_user_sp(const struct syscall_frame *f) { return f->rsp; }

void arch_switch(struct task *prev, struct task *next)
{
    cpu_set_kernel_stack(next->kstack_top);
    if (prev->is_user)
        fpu_save(prev->arch.fpu);
    if (next->is_user) {
        fpu_restore(next->arch.fpu);
        wrmsr(MSR_FS_BASE, next->arch.fs_base);
    }
    if (next->pgd != read_cr3())
        write_cr3(next->pgd);
    switch_context(&prev->arch.rsp, next->arch.rsp);
    /* Back on `prev`'s stack, some time later — possibly on another CPU. */
}

void arch_switch_first(struct task *next)
{
    uint64_t scratch;
    switch_context(&scratch, next->arch.rsp);
    __builtin_unreachable();
}

void arch_exec_enter(struct task *t, uint64_t entry, uint64_t user_sp)
{
    write_cr3(t->pgd);
    wrmsr(MSR_FS_BASE, 0);
    enter_usermode(entry, user_sp, 0, 0);
}
