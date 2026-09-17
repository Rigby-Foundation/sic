/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Signal frames on x86_64: the register set as seen by handlers, the
 * Linux-shaped rt frame musl's restorer expects, and the conversions from
 * the kernel entry frames. */
#include "asm/signal.h"
#include "proc/signal.h"
#include "proc/sched.h"
#include "string.h"

void arch_sigregs_from_syscall(struct sigregs *r, const struct syscall_frame *f)
{
    r->r15 = f->r15; r->r14 = f->r14; r->r13 = f->r13; r->r12 = f->r12;
    r->r11 = f->r11; r->r10 = f->r10; r->r9 = f->r9; r->r8 = f->r8;
    r->rbp = f->rbp; r->rbx = f->rbx; r->rdi = f->rdi; r->rsi = f->rsi;
    r->rdx = f->rdx; r->rcx = f->rcx; r->rax = f->rax;
    r->rip = f->rip; r->rflags = f->rflags; r->rsp = f->rsp;
}

void arch_sigregs_to_syscall(const struct sigregs *r, struct syscall_frame *f)
{
    f->r15 = r->r15; f->r14 = r->r14; f->r13 = r->r13; f->r12 = r->r12;
    f->r11 = r->r11; f->r10 = r->r10; f->r9 = r->r9; f->r8 = r->r8;
    f->rbp = r->rbp; f->rbx = r->rbx; f->rdi = r->rdi; f->rsi = r->rsi;
    f->rdx = r->rdx; f->rcx = r->rcx; f->rax = r->rax;
    f->rip = r->rip; f->rflags = (r->rflags & 0x3C7FD7) | 0x202; f->rsp = r->rsp;
}

void arch_sigregs_from_irq(struct sigregs *r, const struct interrupt_frame *f)
{
    r->r15 = f->r15; r->r14 = f->r14; r->r13 = f->r13; r->r12 = f->r12;
    r->r11 = f->r11; r->r10 = f->r10; r->r9 = f->r9; r->r8 = f->r8;
    r->rbp = f->rbp; r->rbx = f->rbx; r->rdi = f->rdi; r->rsi = f->rsi;
    r->rdx = f->rdx; r->rcx = f->rcx; r->rax = f->rax;
    r->rip = f->rip; r->rflags = f->rflags; r->rsp = f->rsp;
}

void arch_sigregs_to_irq(const struct sigregs *r, struct interrupt_frame *f)
{
    f->r15 = r->r15; f->r14 = r->r14; f->r13 = r->r13; f->r12 = r->r12;
    f->r11 = r->r11; f->r10 = r->r10; f->r9 = r->r9; f->r8 = r->r8;
    f->rbp = r->rbp; f->rbx = r->rbx; f->rdi = r->rdi; f->rsi = r->rsi;
    f->rdx = r->rdx; f->rcx = r->rcx; f->rax = r->rax;
    f->rip = r->rip; f->rflags = (r->rflags & 0x3C7FD7) | 0x202; f->rsp = r->rsp;
}

/* What lands on the user stack. The ucontext/siginfo layouts follow Linux
 * x86_64 so musl-built handlers can look at them; restoration uses `saved`. */
struct abi_siginfo {
    int32_t si_signo, si_errno, si_code;
    int32_t pad;
    uint64_t si_addr;                   /* union: si_addr for faults, si_pid for kill */
    uint8_t rest[128 - 24];
};

struct abi_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp; int32_t ss_flags; int32_t pad0; uint64_t ss_size;
    uint64_t gregs[23];
    uint64_t fpregs;
    uint64_t reserved[8];
    uint64_t uc_sigmask[16];
};

struct sigframe {
    uint64_t retaddr;                   /* the restorer */
    struct abi_siginfo info;
    struct abi_ucontext uc;
    /* private: exact state to resume */
    struct sigregs saved;
    uint64_t saved_mask;
    uint64_t magic;
};
#define SIGFRAME_MAGIC 0x5349474652414d45UL


int arch_signal_setup_frame(struct task *t, int sig, uint64_t fault_addr, uint64_t handler, uint64_t restorer,
                            uint64_t saved_mask, struct sigregs *r)
{
    (void)t;
    uint64_t sp = r->rsp - 128;                       /* skip the red zone */
    sp = (sp - sizeof(struct sigframe)) & ~15UL;
    sp -= 8;                                          /* retaddr slot: sp+8 is 16-aligned */
    if (!user_range_ok(sp, sizeof(struct sigframe)))
        return -1;

    struct sigframe *fr = (struct sigframe *)sp;
    memset(fr, 0, sizeof(*fr));
    fr->retaddr = restorer;
    fr->info.si_signo = sig;
    fr->info.si_addr = fault_addr;
    fr->info.si_code = fault_addr ? 1 : 0;            /* SEGV_MAPERR-ish vs SI_USER */
    fr->uc.gregs[0] = r->r8;  fr->uc.gregs[1] = r->r9;  fr->uc.gregs[2] = r->r10; fr->uc.gregs[3] = r->r11;
    fr->uc.gregs[4] = r->r12; fr->uc.gregs[5] = r->r13; fr->uc.gregs[6] = r->r14; fr->uc.gregs[7] = r->r15;
    fr->uc.gregs[8] = r->rdi; fr->uc.gregs[9] = r->rsi; fr->uc.gregs[10] = r->rbp; fr->uc.gregs[11] = r->rbx;
    fr->uc.gregs[12] = r->rdx; fr->uc.gregs[13] = r->rax; fr->uc.gregs[14] = r->rcx; fr->uc.gregs[15] = r->rsp;
    fr->uc.gregs[16] = r->rip; fr->uc.gregs[17] = r->rflags; fr->uc.gregs[22] = fault_addr;
    fr->uc.uc_sigmask[0] = saved_mask;
    fr->saved = *r;
    fr->saved_mask = saved_mask;
    fr->magic = SIGFRAME_MAGIC;

    /* Run the handler: handler(sig, &info, &uc) on the new stack. */
    r->rip = handler;
    r->rsp = sp;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(uintptr_t)&fr->info;
    r->rdx = (uint64_t)(uintptr_t)&fr->uc;
    r->rax = 0;
    r->rflags &= ~(1UL << 10);                        /* DF clear per ABI */
    return 0;
}

int arch_signal_restore_frame(const struct syscall_frame *f, struct sigregs *r, uint64_t *saved_mask)
{
    struct sigframe *fr = (struct sigframe *)f->rsp;   /* `ret` popped retaddr; rsp points at info */
    fr = (struct sigframe *)((uint8_t *)fr - 8);
    if (!user_range_ok((uint64_t)(uintptr_t)fr, sizeof(*fr)) || fr->magic != SIGFRAME_MAGIC)
        return -1;
    *r = fr->saved;
    *saved_mask = fr->saved_mask;
    return 0;
}
