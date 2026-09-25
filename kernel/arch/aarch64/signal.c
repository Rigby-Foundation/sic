/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Signal frames on aarch64. musl's restorer is `mov x8, #rt_sigreturn; svc`
 * with sp back at the frame; the siginfo/ucontext the handler can look at
 * follow Linux arm64 (the mcontext even carries the SIMD/FP registers as
 * an fpsimd_context), the rest of the frame is our own. */
#include "asm/signal.h"
#include "asm/task.h"
#include "asm/sysreg.h"
#include "proc/signal.h"
#include "proc/sched.h"
#include "string.h"

void arch_sigregs_from_syscall(struct sigregs *r, const struct syscall_frame *f) { r->r = f->r; }
void arch_sigregs_to_syscall(const struct sigregs *r, struct syscall_frame *f)   { f->r = r->r; }
void arch_sigregs_from_irq(struct sigregs *r, const struct interrupt_frame *f)   { r->r = f->r; }
void arch_sigregs_to_irq(const struct sigregs *r, struct interrupt_frame *f)     { f->r = r->r; }

struct abi_siginfo {                    /* 128 bytes, like Linux */
    int32_t si_signo, si_errno, si_code;
    int32_t pad;
    uint64_t si_addr;
    uint8_t rest[128 - 24];
};

struct abi_fpsimd_context {             /* the first record in sigcontext.__reserved */
    uint32_t magic, size;               /* FPSIMD_MAGIC 0x46508001, 528 */
    uint32_t fpsr, fpcr;
    uint8_t vregs[32][16];
};

struct abi_sigcontext {                 /* Linux arm64 */
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
    uint8_t __reserved[4096] __attribute__((aligned(16)));
};

struct abi_ucontext {                   /* Linux arm64 */
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp; int32_t ss_flags; int32_t pad0; uint64_t ss_size;
    uint64_t uc_sigmask[16];            /* 128 bytes */
    struct abi_sigcontext uc_mcontext;
};

struct sigframe {
    struct abi_siginfo info;
    struct abi_ucontext uc;
    struct pt_regs saved;
    struct fpsimd_state fp;
    uint64_t saved_mask;
    uint64_t magic;
};
#define SIGFRAME_MAGIC 0x5349474641524d36ULL

int arch_signal_setup_frame(struct task *t, int sig, uint64_t fault_addr, uint64_t handler, uint64_t restorer,
                            uint64_t saved_mask, struct sigregs *r)
{
    uint64_t sp = (r->r.sp - sizeof(struct sigframe)) & ~15ULL;
    if (!user_range_ok(sp, sizeof(struct sigframe)))
        return -1;
    struct sigframe *fr = (struct sigframe *)(uintptr_t)sp;
    memset(fr, 0, sizeof(*fr));
    fr->info.si_signo = sig;
    fr->info.si_addr = fault_addr;
    fr->info.si_code = fault_addr ? 1 : 0;
    memcpy(fr->uc.uc_mcontext.regs, r->r.x, sizeof(fr->uc.uc_mcontext.regs));
    fr->uc.uc_mcontext.sp = r->r.sp;
    fr->uc.uc_mcontext.pc = r->r.pc;
    fr->uc.uc_mcontext.pstate = r->r.pstate;
    fr->uc.uc_mcontext.fault_address = fault_addr;
    fr->uc.uc_sigmask[0] = saved_mask;
    /* The interrupted code's SIMD/FP registers, live in the hardware. */
    fpsimd_save(&t->arch.fp);
    fr->fp = t->arch.fp;
    struct abi_fpsimd_context *fc = (struct abi_fpsimd_context *)fr->uc.uc_mcontext.__reserved;
    fc->magic = 0x46508001; fc->size = sizeof *fc;
    fc->fpsr = t->arch.fp.fpsr; fc->fpcr = t->arch.fp.fpcr;
    memcpy(fc->vregs, t->arch.fp.v, sizeof fc->vregs);
    fr->saved = r->r;
    fr->saved_mask = saved_mask;
    fr->magic = SIGFRAME_MAGIC;

    /* handler(sig, &info, &uc); returning runs the restorer via x30 */
    r->r.pc = handler;
    r->r.x[30] = restorer;
    r->r.sp = sp;
    r->r.x[0] = (uint64_t)sig;
    r->r.x[1] = (uint64_t)(uintptr_t)&fr->info;
    r->r.x[2] = (uint64_t)(uintptr_t)&fr->uc;
    r->r.pstate = 0;
    return 0;
}

int arch_signal_restore_frame(const struct syscall_frame *f, struct sigregs *r, uint64_t *saved_mask)
{
    struct sigframe *fr = (struct sigframe *)(uintptr_t)f->r.sp;
    if (!user_range_ok((uint64_t)(uintptr_t)fr, sizeof(*fr)) || fr->magic != SIGFRAME_MAGIC)
        return -1;
    r->r = fr->saved;
    *saved_mask = fr->saved_mask;
    struct task *t = task_current();
    t->arch.fp = fr->fp;
    fpsimd_restore(&t->arch.fp);
    return 0;
}
