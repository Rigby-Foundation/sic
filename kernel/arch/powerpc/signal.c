/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Signal frames on 32-bit PowerPC. musl's powerpc restorer is a plain
 * `sc` with __NR_rt_sigreturn; the frame layout below is our own except for
 * the siginfo/ucontext the handler can look at, which follow Linux ppc32. */
#include "asm/signal.h"
#include "asm/irqflags.h"
#include "proc/signal.h"
#include "proc/sched.h"
#include "string.h"

void arch_sigregs_from_syscall(struct sigregs *r, const struct syscall_frame *f) { r->r = f->r; }
void arch_sigregs_to_syscall(const struct sigregs *r, struct syscall_frame *f)   { f->r = r->r; }
void arch_sigregs_from_irq(struct sigregs *r, const struct interrupt_frame *f)   { r->r = f->r; }
void arch_sigregs_to_irq(const struct sigregs *r, struct interrupt_frame *f)     { f->r = r->r; }

struct abi_siginfo {                    /* 128 bytes, like Linux */
    int32_t si_signo, si_errno, si_code;
    uint32_t si_addr;
    uint8_t rest[128 - 16];
};

struct abi_mcontext {                   /* Linux ppc32 struct pt_regs shape: 44 words */
    uint32_t gpr[32];
    uint32_t nip, msr, orig_gpr3, ctr, link, xer, ccr, mq, trap, dar, dsisr, result;
};

struct abi_ucontext {                   /* Linux ppc32 ucontext */
    uint32_t uc_flags, uc_link;
    uint32_t ss_sp; int32_t ss_flags; uint32_t ss_size;
    int32_t uc_pad;
    uint32_t uc_regs;                   /* pointer to uc_mcontext */
    uint32_t uc_sigmask[2];
    int32_t uc_maskext[30];
    int32_t uc_pad2[3];
    struct abi_mcontext uc_mcontext;
};

struct sigframe {
    uint32_t back_chain, lr_save;       /* ABI stack frame the handler may use */
    struct abi_siginfo info;
    struct abi_ucontext uc;
    struct pt_regs saved;
    uint64_t saved_mask;
    uint32_t magic;
    uint32_t pad;
    /* The interrupted code's FP and vector registers, when it had them
     * live; the handler starts with both units disabled and gets its own
     * copies through the unavailable traps. */
    uint64_t fpr[32];
    uint32_t fpscr, has_fp, has_vec, pad2;
    struct ppc_vec_state vec;
};
#define SIGFRAME_MAGIC 0x53494746u

int arch_signal_setup_frame(struct task *t, int sig, uint64_t fault_addr, uint64_t handler, uint64_t restorer,
                            uint64_t saved_mask, struct sigregs *r)
{
    (void)t;
    uint32_t sp = (r->r.gpr[1] - sizeof(struct sigframe)) & ~15u;
    if (!user_range_ok(sp, sizeof(struct sigframe)))
        return -1;
    struct sigframe *fr = (struct sigframe *)(uintptr_t)sp;
    memset(fr, 0, sizeof(*fr));
    fr->back_chain = r->r.gpr[1];
    fr->info.si_signo = sig;
    fr->info.si_addr = (uint32_t)fault_addr;
    fr->info.si_code = fault_addr ? 1 : 0;
    memcpy(fr->uc.uc_mcontext.gpr, r->r.gpr, sizeof(fr->uc.uc_mcontext.gpr));
    fr->uc.uc_mcontext.nip = r->r.nip;
    fr->uc.uc_mcontext.msr = r->r.msr;
    fr->uc.uc_mcontext.ctr = r->r.ctr;
    fr->uc.uc_mcontext.link = r->r.link;
    fr->uc.uc_mcontext.xer = r->r.xer;
    fr->uc.uc_mcontext.ccr = r->r.ccr;
    fr->uc.uc_mcontext.dar = (uint32_t)fault_addr;
    fr->uc.uc_regs = (uint32_t)(uintptr_t)&fr->uc.uc_mcontext;
    fr->uc.uc_sigmask[0] = (uint32_t)saved_mask;
    fr->uc.uc_sigmask[1] = (uint32_t)(saved_mask >> 32);
    fr->saved = r->r;
    fr->saved_mask = saved_mask;
    fr->magic = SIGFRAME_MAGIC;
    fr->has_fp = t->arch.fp_used && (r->r.msr & MSR_FP);
    fr->has_vec = t->arch.vec_used && (r->r.msr & MSR_VEC);
    if (fr->has_fp) {
        ppc_fpu_save_for_signal(t, r->r.msr);
        memcpy(fr->fpr, t->arch.fpr, sizeof(fr->fpr));
        fr->fpscr = t->arch.fpscr;
    }
    if (fr->has_vec) {
        ppc_vec_save_for_signal(t, r->r.msr);
        fr->vec = t->arch.vec;
    }

    /* handler(sig, &info, &uc); returning runs the restorer via LR */
    r->r.nip = (uint32_t)handler;
    r->r.link = (uint32_t)restorer;
    r->r.gpr[1] = sp;
    r->r.gpr[3] = (uint32_t)sig;
    r->r.gpr[4] = (uint32_t)(uintptr_t)&fr->info;
    r->r.gpr[5] = (uint32_t)(uintptr_t)&fr->uc;
    r->r.msr &= ~(MSR_FP | MSR_VEC);    /* the handler starts without FP/vector state loaded */
    return 0;
}

int arch_signal_restore_frame(const struct syscall_frame *f, struct sigregs *r, uint64_t *saved_mask)
{
    /* The restorer runs with r1 still at our frame (handlers restore the
     * stack pointer before returning through LR). */
    struct sigframe *fr = (struct sigframe *)(uintptr_t)f->r.gpr[1];
    if (!user_range_ok((uint64_t)(uintptr_t)fr, sizeof(*fr)) || fr->magic != SIGFRAME_MAGIC)
        return -1;
    r->r = fr->saved;
    *saved_mask = fr->saved_mask;
    /* Put the interrupted state back into arch; the unavailable traps reload
     * it on the next use (the handler may have clobbered the hardware). */
    struct task *t = task_current();
    if (fr->has_fp) {
        memcpy(t->arch.fpr, fr->fpr, sizeof(t->arch.fpr));
        t->arch.fpscr = fr->fpscr;
    }
    if (fr->has_vec)
        t->arch.vec = fr->vec;
    r->r.msr &= ~(MSR_FP | MSR_VEC);
    return 0;
}
