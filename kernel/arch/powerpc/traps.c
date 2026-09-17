/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Exception dispatch: page faults, external interrupts (OpenPIC), the
 * decrementer, system calls, and the fatal rest. */
#include "asm/ptrace.h"
#include "asm/irq.h"
#include "asm/cpu.h"
#include "asm/timer.h"
#include "asm/memlayout.h"
#include "asm/of.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "proc/signal.h"
#include "printf.h"
#include "string.h"

extern char ppc_vectors_start[], ppc_vectors_end[];
extern int  ppc_mmu_fault(uint32_t addr, int write, int user);      /* mmu.c: 0 = fixed up */
extern void ppc_openpic_dispatch(struct interrupt_frame *f);        /* openpic.c */
extern void ppc_decrementer(struct interrupt_frame *f);             /* timer.c */

void ppc_install_vectors(void)
{
    /* Physical 0 is the start of the direct map. */
    memcpy(P2V(0), ppc_vectors_start, (size_t)(ppc_vectors_end - ppc_vectors_start));
    /* Instruction cache: the copied code must be visible to fetches. */
    for (uintptr_t a = (uintptr_t)P2V(0); a < (uintptr_t)P2V(0) + 0x3000; a += 32)
        __asm__ volatile("dcbst 0, %0; sync; icbi 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync; isync");
}

static const char *trap_name(uint32_t t)
{
    switch (t) {
    case 0x100: return "System Reset";
    case 0x200: return "Machine Check";
    case 0x300: return "Data Storage";
    case 0x400: return "Instruction Storage";
    case 0x500: return "External Interrupt";
    case 0x600: return "Alignment";
    case 0x700: return "Program";
    case 0x800: return "FP Unavailable";
    case 0x900: return "Decrementer";
    case 0xC00: return "System Call";
    case 0xD00: return "Trace";
    case 0xF20: return "AltiVec Unavailable";
    default:    return "Unknown";
    }
}

static void fatal(struct pt_regs *r, const char *why)
{
    static volatile int in_fatal;
    if (__atomic_exchange_n(&in_fatal, 1, __ATOMIC_SEQ_CST)) {
        kprintf("\n*** nested exception 0x%x at %x: halted ***\n", r->trap, r->nip);
        for (;;) interrupts_disable();
    }
    kprintf("\n*** EXCEPTION 0x%x: %s%s ***\n", r->trap, trap_name(r->trap), why);
    kprintf("nip=");
    kprint_sym(r->nip);
    kprintf(" msr=%08x lr=%08x ctr=%08x xer=%08x cr=%08x\n", r->msr, r->link, r->ctr, r->xer, r->ccr);
    kprintf("dar=%08x dsisr=%08x\n", r->dar, r->dsisr);
    for (int i = 0; i < 32; i += 4)
        kprintf("r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n", i, r->gpr[i], i + 1, r->gpr[i + 1], i + 2, r->gpr[i + 2], i + 3, r->gpr[i + 3]);
    kprintf("backtrace:\n");
    uint32_t *sp = (uint32_t *)(uintptr_t)r->gpr[1];
    for (int i = 0; i < 16 && sp; i++) {
        uintptr_t p = (uintptr_t)sp;
        if (p < HHDM_BASE || p >= HHDM_BASE + HHDM_SIZE || (p & 3)) break;
        uint32_t ret = sp[1];
        if (ret < HHDM_BASE) break;
        kprintf("  ");
        kprint_sym(ret);
        kprintf("\n");
        sp = (uint32_t *)(uintptr_t)sp[0];
    }
    kprintf("task %s; system halted.\n", task_current() ? task_current()->name : "?");
    for (;;) interrupts_disable();
}

void ppc_trap(struct pt_regs *r)
{
    int user = (r->msr & MSR_PR) != 0;
    struct interrupt_frame *f = (struct interrupt_frame *)r;

    switch (r->trap) {
    case 0x900:
        ppc_decrementer(f);
        break;
    case 0x500:
        ppc_openpic_dispatch(f);
        break;
    case 0xC00: {
        /* SRR0 already points past the sc (unlike the other exceptions). */
        interrupts_enable();
        syscall_dispatch((struct syscall_frame *)r);
        interrupts_disable();
        return;
    }
    case 0x300:                                         /* data access */
    case 0x400: {                                       /* instruction fetch */
        uint32_t addr = r->trap == 0x300 ? r->dar : r->nip;
        int write = r->trap == 0x300 && (r->dsisr & 0x02000000);
        if (ppc_mmu_fault(addr, write, user) == 0)
            break;                                      /* hash table refilled from the page tables */
        if (user) {
#ifdef CONFIG_SIGNALS
            if (signal_fault(f, SIGSEGV, addr))
                break;
#endif
            kprintf("[%s (pid %u) killed: %s at nip=%x, addr=%x, dsisr=%x]\n",
                    task_current()->name, task_current()->id, trap_name(r->trap), r->nip, addr, r->dsisr);
            task_current()->killed_sig = SIGSEGV;
            task_exit_code(128 + SIGSEGV);
        }
        fatal(r, "");
    }
    case 0x800:                                         /* FP unavailable: give the task the FPU */
        if (user) {
            extern void ppc_fpu_enable_for(struct task *t, struct pt_regs *r);
            ppc_fpu_enable_for(task_current(), r);
            break;
        }
        fatal(r, " (floating point in the kernel)");
    case 0xF20:                                         /* AltiVec unavailable: likewise */
        if (user) {
            extern void ppc_vec_enable_for(struct task *t, struct pt_regs *r);
            ppc_vec_enable_for(task_current(), r);
            break;
        }
        fatal(r, " (AltiVec in the kernel)");
    case 0x600: case 0x700:
        if (user) {
            int sig = r->trap == 0x600 ? SIGBUS : SIGILL;
#ifdef CONFIG_SIGNALS
            if (signal_fault(f, sig, r->nip))
                break;
#endif
            kprintf("[%s (pid %u) killed: %s at nip=%x]\n", task_current()->name, task_current()->id, trap_name(r->trap), r->nip);
            task_current()->killed_sig = sig;
            task_exit_code(128 + sig);
        }
        fatal(r, "");
    default:
        fatal(r, "");
    }
#ifdef CONFIG_SIGNALS
    if (user)
        signal_deliver_irq(f);
#endif
}
