/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Synchronous exceptions: system calls, page faults, and the fatal rest.
 * Interrupts arrive in gic.c. */
#include "asm/ptrace.h"
#include "asm/sysreg.h"
#include "asm/irqflags.h"
#include "asm/memlayout.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "proc/signal.h"
#include "proc/elf.h"
#include "printf.h"

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN:   return "Unknown reason (undefined instruction)";
    case EC_WFI:       return "WFI/WFE trap";
    case EC_FP_ASIMD:  return "SIMD/FP access";
    case EC_SVC64:     return "SVC";
    case EC_IABT_LOW:  case EC_IABT_CUR: return "Instruction abort";
    case EC_PC_ALIGN:  return "PC alignment";
    case EC_DABT_LOW:  case EC_DABT_CUR: return "Data abort";
    case EC_SP_ALIGN:  return "SP alignment";
    case EC_BRK64:     return "BRK";
    default:           return "Exception";
    }
}

static void fatal(struct pt_regs *r, const char *why)
{
    static volatile int in_fatal;
    if (__atomic_exchange_n(&in_fatal, 1, __ATOMIC_SEQ_CST)) {
        kprintf("\n*** nested exception esr=%llx at %llx: halted ***\n", r->esr, r->pc);
        for (;;) interrupts_disable();
    }
    kprintf("\n*** EXCEPTION: %s (esr %llx)%s ***\n", ec_name(ESR_EC(r->esr)), r->esr, why);
    kprintf("pc=");
    kprint_sym(r->pc);
    kprintf(" sp=%llx pstate=%llx far=%llx\n", r->sp, r->pstate, r->far);
    for (int i = 0; i < 30; i += 3)
        kprintf("x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n", i, r->x[i], i + 1, r->x[i + 1], i + 2, r->x[i + 2]);
    kprintf("x30=%016llx\n", r->x[30]);
    kprintf("backtrace:\n");
    uint64_t *fp = (uint64_t *)(uintptr_t)r->x[29];
    for (int i = 0; i < 16 && fp; i++) {
        uintptr_t p = (uintptr_t)fp;
        if (p < HHDM_BASE || p >= HHDM_BASE + HHDM_SIZE || (p & 7)) break;
        uint64_t ret = fp[1];
        if (ret < HHDM_BASE) break;
        kprintf("  ");
        kprint_sym(ret);
        kprintf("\n");
        fp = (uint64_t *)(uintptr_t)fp[0];
    }
    kprintf("task %s; system halted.\n", task_current() ? task_current()->name : "?");
    for (;;) interrupts_disable();
}

static void kill_user(struct pt_regs *r, int sig, uint64_t addr)
{
#ifdef CONFIG_SIGNALS
    if (signal_fault((struct interrupt_frame *)r, sig, addr))
        return;
#endif
    kprintf("[%s (pid %u) killed: %s at pc=%llx, addr=%llx, esr=%llx]\n",
            task_current()->name, task_current()->id, ec_name(ESR_EC(r->esr)), r->pc, addr, r->esr);
    kprintf("  sp=%llx x0=%llx x1=%llx x2=%llx x8=%llx x30=%llx\n", r->sp, r->x[0], r->x[1], r->x[2], r->x[8], r->x[30]);
    kprintf("  x0..x30:");
    for (int i = 0; i < 31; i++) kprintf(" %llx", r->x[i]);
    kprintf("\n");
    if (user_ok(r->sp, 16 * sizeof(uint64_t))) {
        const uint64_t *sp = (const uint64_t *)(uintptr_t)r->sp;
        kprintf("  stack:");
        for (int i = 0; i < 16; i++) kprintf(" %llx", sp[i]);
        kprintf("\n");
    }
    task_print_trace(task_current());
    task_current()->killed_sig = sig;
    task_exit_code(128 + sig);
}

void aarch64_sync(struct pt_regs *r, int user)
{
    uint32_t ec = ESR_EC(r->esr);
    switch (ec) {
    case EC_SVC64:
        /* ELR already points past the svc. */
        interrupts_enable();
        syscall_dispatch((struct syscall_frame *)r);
        interrupts_disable();
        return;
    case EC_DABT_LOW:
    case EC_IABT_LOW: {
        uint64_t addr = r->far;
        interrupts_enable();
        if (process_grow_stack(task_current(), addr) == 0) { interrupts_disable(); break; }
        kill_user(r, SIGSEGV, addr);
        break;
    }
    case EC_DABT_CUR:
    case EC_IABT_CUR:
        fatal(r, " (in the kernel)");
    case EC_UNKNOWN:
    case EC_BRK64:
        if (user) { interrupts_enable(); kill_user(r, ec == EC_BRK64 ? SIGTRAP : SIGILL, r->pc); break; }
        fatal(r, "");
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        if (user) { interrupts_enable(); kill_user(r, SIGBUS, r->pc); break; }
        fatal(r, "");
    case EC_FP_ASIMD:
        if (user) { interrupts_enable(); kill_user(r, SIGILL, r->pc); break; }
        fatal(r, " (SIMD/FP in the kernel)");
    default:
        if (user) { interrupts_enable(); kill_user(r, SIGILL, r->pc); break; }
        fatal(r, "");
    }
#ifdef CONFIG_SIGNALS
    if (user) {
        interrupts_disable();
        signal_deliver_irq((struct interrupt_frame *)r);
    }
#endif
    interrupts_disable();
}

void aarch64_bad(struct pt_regs *r)
{
    fatal(r, " (unexpected vector)");
}
