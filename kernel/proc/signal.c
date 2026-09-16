/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* POSIX signals. A signal is a bit in task->sig_pending; delivery happens on
 * the way back to user mode by rewriting the return frame to run the handler
 * on the user stack, with a Linux-shaped rt frame so musl's restorer works. */
#include "proc/signal.h"
#include "proc/sched.h"
#include "arch/x86_64/timer.h"
#include "mm/vmm.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"
#include "mm/heap.h"

#define BIT(s) (1UL << ((s) - 1))
#define UNCATCHABLE (BIT(SIGKILL) | BIT(SIGSTOP))

/* ---- per-task state ---------------------------------------------------------- */

#define ACT(t, sig) ((t)->sighand->actions[(sig) - 1])

int signal_init_task(struct task *t)
{
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_mask_valid = 0;
    t->sighand = kzalloc(sizeof(*t->sighand));
    if (!t->sighand)
        return -ENOMEM;
    t->sighand->refs = 1;
    return 0;
}

int signal_fork(struct task *child, struct task *parent)
{
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    child->sig_saved_mask_valid = 0;
    struct sighand *old = child->sighand;
    child->sighand = kzalloc(sizeof(*child->sighand));
    if (!child->sighand) {
        child->sighand = old;
        return -ENOMEM;
    }
    if (old && __atomic_sub_fetch(&old->refs, 1, __ATOMIC_SEQ_CST) == 0)
        kfree(old);
    memcpy(child->sighand->actions, parent->sighand->actions, sizeof(child->sighand->actions));
    child->sighand->refs = 1;
    return 0;
}

void signal_clone(struct task *child, struct task *parent)
{
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    child->sig_saved_mask_valid = 0;
    struct sighand *old = child->sighand;
    child->sighand = parent->sighand;
    __atomic_add_fetch(&child->sighand->refs, 1, __ATOMIC_SEQ_CST);
    if (old && __atomic_sub_fetch(&old->refs, 1, __ATOMIC_SEQ_CST) == 0)
        kfree(old);
}

void signal_release(struct task *t)
{
    if (t->sighand && __atomic_sub_fetch(&t->sighand->refs, 1, __ATOMIC_SEQ_CST) == 0)
        kfree(t->sighand);
    t->sighand = NULL;
}

void signal_exec(struct task *t)
{
    for (int i = 0; i < NSIG; i++)
        if (t->sighand->actions[i].handler != SIG_IGN)
            memset(&t->sighand->actions[i], 0, sizeof(t->sighand->actions[i]));
        else
            t->sighand->actions[i].flags = 0;
}

static int default_ignored(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT ||
           sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

int task_send_signal(struct task *t, int sig)
{
    if (sig <= 0 || sig > NSIG)
        return -EINVAL;
    if (!t->is_user)
        return -EPERM;
    /* Ignored signals are discarded at send time (as on Linux), so they never
     * interrupt a sleep or a wait. */
    uint64_t h = ACT(t, sig).handler;
    if (!(BIT(sig) & UNCATCHABLE) && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig))))
        return 0;
    __atomic_fetch_or(&t->sig_pending, BIT(sig), __ATOMIC_SEQ_CST);
    task_wake(t);                       /* interrupts a sleep or a blocking read */
    return 0;
}

int task_signal_pending(struct task *t)
{
    return t->group_exit || (t->sig_pending & ~(t->sig_blocked & ~UNCATCHABLE)) != 0;
}

/* ---- frames --------------------------------------------------------------------- */

static void regs_from_syscall(struct sigregs *r, const struct syscall_frame *f)
{
    r->r15 = f->r15; r->r14 = f->r14; r->r13 = f->r13; r->r12 = f->r12;
    r->r11 = f->r11; r->r10 = f->r10; r->r9 = f->r9; r->r8 = f->r8;
    r->rbp = f->rbp; r->rbx = f->rbx; r->rdi = f->rdi; r->rsi = f->rsi;
    r->rdx = f->rdx; r->rcx = f->rcx; r->rax = f->rax;
    r->rip = f->rip; r->rflags = f->rflags; r->rsp = f->rsp;
}

static void regs_to_syscall(const struct sigregs *r, struct syscall_frame *f)
{
    f->r15 = r->r15; f->r14 = r->r14; f->r13 = r->r13; f->r12 = r->r12;
    f->r11 = r->r11; f->r10 = r->r10; f->r9 = r->r9; f->r8 = r->r8;
    f->rbp = r->rbp; f->rbx = r->rbx; f->rdi = r->rdi; f->rsi = r->rsi;
    f->rdx = r->rdx; f->rcx = r->rcx; f->rax = r->rax;
    f->rip = r->rip; f->rflags = (r->rflags & 0x3C7FD7) | 0x202; f->rsp = r->rsp;
}

static void regs_from_irq(struct sigregs *r, const struct interrupt_frame *f)
{
    r->r15 = f->r15; r->r14 = f->r14; r->r13 = f->r13; r->r12 = f->r12;
    r->r11 = f->r11; r->r10 = f->r10; r->r9 = f->r9; r->r8 = f->r8;
    r->rbp = f->rbp; r->rbx = f->rbx; r->rdi = f->rdi; r->rsi = f->rsi;
    r->rdx = f->rdx; r->rcx = f->rcx; r->rax = f->rax;
    r->rip = f->rip; r->rflags = f->rflags; r->rsp = f->rsp;
}

static void regs_to_irq(const struct sigregs *r, struct interrupt_frame *f)
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

static int user_range_ok(uint64_t p, uint64_t len)
{
    if (p < USER_BASE || p + len > USER_END) return 0;
    uint64_t pml4 = task_current()->mm->pml4;
    for (uint64_t a = p & ~0xFFFUL; a < p + len; a += 4096)
        if (!vmm_translate_in(pml4, a))
            return 0;
    return 1;
}

/* Pick the next deliverable signal, or 0. */
static int next_signal(struct task *t)
{
    uint64_t deliverable = t->sig_pending & ~(t->sig_blocked & ~UNCATCHABLE);
    if (!deliverable)
        return 0;
    return __builtin_ctzl(deliverable) + 1;
}

/* Rewrite `r` (the state we'd return to) so the handler runs first.
 * Returns 1 if a handler was set up, 0 if the signal was consumed otherwise. */
static int setup_frame(struct task *t, int sig, uint64_t fault_addr, struct sigregs *r)
{
    struct k_sigaction *a = &ACT(t, sig);
    uint64_t saved_mask = t->sig_saved_mask_valid ? t->sig_saved_mask : t->sig_blocked;
    t->sig_saved_mask_valid = 0;

    uint64_t sp = r->rsp - 128;                       /* skip the red zone */
    sp = (sp - sizeof(struct sigframe)) & ~15UL;
    sp -= 8;                                          /* retaddr slot: sp+8 is 16-aligned */
    if (!user_range_ok(sp, sizeof(struct sigframe))) {
        kprintf("[%s (pid %u): no user stack for signal %d, killing]\n", t->name, t->id, sig);
        t->killed_sig = SIGSEGV;
        task_exit_code(128 + SIGSEGV);
    }

    struct sigframe *fr = (struct sigframe *)sp;
    memset(fr, 0, sizeof(*fr));
    fr->retaddr = a->restorer;
    fr->info.si_signo = sig;
    fr->info.si_addr = fault_addr;
    fr->info.si_code = fault_addr ? 1 : 0;            /* SEGV_MAPERR-ish vs SI_USER */
    static const int greg_order[] = { 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22 };
    (void)greg_order;
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
    r->rip = a->handler;
    r->rsp = sp;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)&fr->info;
    r->rdx = (uint64_t)&fr->uc;
    r->rax = 0;
    r->rflags &= ~(1UL << 10);                        /* DF clear per ABI */

    t->sig_blocked |= (((uint64_t)a->mask[1] << 32) | a->mask[0]);
    if (!(a->flags & SA_NODEFER))
        t->sig_blocked |= BIT(sig);
    t->sig_blocked &= ~UNCATCHABLE;
    if (a->flags & SA_RESETHAND)
        memset(a, 0, sizeof(*a));
    return 1;
}

/* Core: consume pending signals until one needs a handler frame (or none left). */
static void deliver(struct sigregs *r, uint64_t fault_addr)
{
    struct task *t = task_current();
    int sig;
    if (t->group_exit)
        task_exit_code(t->exit_code);       /* another thread called exit_group */
    while ((sig = next_signal(t)) != 0) {
        __atomic_fetch_and(&t->sig_pending, ~BIT(sig), __ATOMIC_SEQ_CST);
        struct k_sigaction *a = &ACT(t, sig);
        uint64_t h = (BIT(sig) & UNCATCHABLE) ? SIG_DFL : a->handler;

        if (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig)))
            continue;
        if (h == SIG_DFL) {
            kprintf("[%s (pid %u) terminated by signal %d]\n", t->name, t->tgid, sig);
            t->killed_sig = sig;
            task_exit_group(128 + sig);
        }
        if (!(a->flags & SA_RESTORER)) {
            kprintf("[%s (pid %u): handler for signal %d without SA_RESTORER, killing]\n", t->name, t->id, sig);
            t->killed_sig = sig;
            task_exit_code(128 + sig);
        }
        if (setup_frame(t, sig, fault_addr, r))
            return;                     /* one handler at a time; the rest wait for sigreturn */
    }
}

void signal_deliver_syscall(struct syscall_frame *f)
{
    if (!task_signal_pending(task_current()))
        return;
    struct sigregs r;
    regs_from_syscall(&r, f);
    deliver(&r, 0);
    regs_to_syscall(&r, f);
}

void signal_deliver_irq(struct interrupt_frame *f)
{
    if (!(f->cs & 3) || !task_signal_pending(task_current()))
        return;
    struct sigregs r;
    regs_from_irq(&r, f);
    deliver(&r, 0);
    regs_to_irq(&r, f);
}

int signal_fault(struct interrupt_frame *f, int sig, uint64_t addr)
{
    struct task *t = task_current();
    struct k_sigaction *a = &ACT(t, sig);
    /* A fault with the signal blocked or ignored is fatal, as on Linux. */
    if (a->handler == SIG_DFL || a->handler == SIG_IGN || (t->sig_blocked & BIT(sig)))
        return 0;
    __atomic_fetch_or(&t->sig_pending, BIT(sig), __ATOMIC_SEQ_CST);
    struct sigregs r;
    regs_from_irq(&r, f);
    deliver(&r, addr);
    regs_to_irq(&r, f);
    return 1;
}

/* ---- system calls ------------------------------------------------------------------ */

long sys_rt_sigaction(int sig, uint64_t uact, uint64_t uoact, uint64_t setsize)
{
    struct task *t = task_current();
    if (setsize != 8 || sig < 1 || sig > NSIG) return -EINVAL;
    if (uoact) {
        if (!user_range_ok(uoact, sizeof(struct k_sigaction))) return -EFAULT;
        memcpy((void *)uoact, &ACT(t, sig), sizeof(struct k_sigaction));
    }
    if (uact) {
        if (!user_range_ok(uact, sizeof(struct k_sigaction))) return -EFAULT;
        if (BIT(sig) & UNCATCHABLE) return -EINVAL;
        memcpy(&ACT(t, sig), (const void *)uact, sizeof(struct k_sigaction));
    }
    return 0;
}

long sys_rt_sigprocmask(int how, uint64_t uset, uint64_t uoset, uint64_t setsize)
{
    struct task *t = task_current();
    if (setsize != 8) return -EINVAL;
    if (uoset) {
        if (!user_range_ok(uoset, 8)) return -EFAULT;
        *(uint64_t *)uoset = t->sig_blocked;
    }
    if (uset) {
        if (!user_range_ok(uset, 8)) return -EFAULT;
        uint64_t set = *(const uint64_t *)uset;
        switch (how) {
        case SIG_BLOCK:   t->sig_blocked |= set; break;
        case SIG_UNBLOCK: t->sig_blocked &= ~set; break;
        case SIG_SETMASK: t->sig_blocked = set; break;
        default: return -EINVAL;
        }
        t->sig_blocked &= ~UNCATCHABLE;
    }
    return 0;
}

long sys_rt_sigpending(uint64_t uset, uint64_t setsize)
{
    if (setsize != 8 || !user_range_ok(uset, 8)) return -EINVAL;
    *(uint64_t *)uset = task_current()->sig_pending;
    return 0;
}

long sys_pause(void)
{
    struct task *t = task_current();
    while (!task_signal_pending(t))
        task_block();
    return -EINTR;
}

long sys_rt_sigsuspend(uint64_t uset, uint64_t setsize)
{
    struct task *t = task_current();
    if (setsize != 8 || !user_range_ok(uset, 8)) return -EINVAL;
    uint64_t old = t->sig_blocked;
    t->sig_blocked = *(const uint64_t *)uset & ~UNCATCHABLE;
    while (!task_signal_pending(t))
        task_block();
    /* The handler frame must restore the pre-suspend mask, not the temporary one. */
    t->sig_saved_mask = old;
    t->sig_saved_mask_valid = 1;
    return -EINTR;
}

long sys_rt_sigreturn(struct syscall_frame *f)
{
    struct task *t = task_current();
    struct sigframe *fr = (struct sigframe *)f->rsp;   /* `ret` popped retaddr; rsp points at info */
    fr = (struct sigframe *)((uint8_t *)fr - 8);
    if (!user_range_ok((uint64_t)fr, sizeof(*fr)) || fr->magic != SIGFRAME_MAGIC) {
        kprintf("[%s (pid %u): bad sigreturn frame, killing]\n", t->name, t->id);
        t->killed_sig = SIGSEGV;
        task_exit_code(128 + SIGSEGV);
    }
    struct sigregs r = fr->saved;
    t->sig_blocked = fr->saved_mask & ~UNCATCHABLE;
    regs_to_syscall(&r, f);
    return 0;
}

static void kill_all_cb(struct task *t, void *arg)
{
    struct { struct task *self; int sig; int any; } *ctx = arg;
    if (!t->is_user || t->is_thread || t->tgid == ctx->self->tgid || t->state == TASK_ZOMBIE)
        return;
    if (ctx->sig) {
        __atomic_fetch_or(&t->sig_pending, BIT(ctx->sig), __ATOMIC_SEQ_CST);
        t->wake_pending = 1;            /* task_wake would take the lock we hold */
    }
    ctx->any = 1;
}

long sys_tkill(long tid, int sig)
{
    if (sig < 0 || sig > NSIG) return -EINVAL;
    struct task *t = task_find((uint32_t)tid);
    if (!t || !t->is_user || t->state == TASK_ZOMBIE) return -ESRCH;
    return sig ? task_send_signal(t, sig) : 0;
}

long sys_kill(long pid, int sig)
{
    struct task *self = task_current();
    if (sig < 0 || sig > NSIG) return -EINVAL;
    if (pid > 0 || pid == 0) {
        /* A process is addressed by its thread group id: signal the leader. */
        struct task *t = pid == 0 ? task_find(self->tgid) : task_find((uint32_t)pid);
        if (!t || !t->is_user || t->is_thread || t->state == TASK_ZOMBIE) return -ESRCH;
        return sig ? task_send_signal(t, sig) : 0;
    }
    /* -1: everyone we may signal, except ourselves. Marking under the
     * scheduler lock; the wakeups happen afterwards. */
    struct { struct task *self; int sig; int any; } ctx = { self, sig, 0 };
    task_foreach(kill_all_cb, &ctx);
    return ctx.any ? 0 : -ESRCH;
}

long sys_alarm(uint64_t secs)
{
    struct task *t = task_current();
    uint64_t now = timer_ticks();
    long left = t->alarm_at && t->alarm_at > now ? (long)((t->alarm_at - now + TIMER_HZ - 1) / TIMER_HZ) : 0;
    t->alarm_at = secs ? now + secs * TIMER_HZ : 0;
    t->alarm_interval = 0;
    return left;
}

struct abi_timeval { int64_t tv_sec, tv_usec; };
struct abi_itimerval { struct abi_timeval it_interval, it_value; };

static uint64_t tv_to_ticks(const struct abi_timeval *tv)
{
    return (uint64_t)tv->tv_sec * TIMER_HZ + (uint64_t)tv->tv_usec * TIMER_HZ / 1000000;
}

static void ticks_to_tv(uint64_t ticks, struct abi_timeval *tv)
{
    tv->tv_sec = (int64_t)(ticks / TIMER_HZ);
    tv->tv_usec = (int64_t)((ticks % TIMER_HZ) * 1000000 / TIMER_HZ);
}

long sys_getitimer(int which, uint64_t uold)
{
    struct task *t = task_current();
    if (which != 0) return -EINVAL;                 /* ITIMER_REAL only */
    if (!user_range_ok(uold, sizeof(struct abi_itimerval))) return -EFAULT;
    struct abi_itimerval *o = (void *)uold;
    uint64_t now = timer_ticks();
    ticks_to_tv(t->alarm_at > now ? t->alarm_at - now : 0, &o->it_value);
    ticks_to_tv(t->alarm_interval, &o->it_interval);
    return 0;
}

long sys_setitimer(int which, uint64_t unew, uint64_t uold)
{
    struct task *t = task_current();
    if (which != 0) return -EINVAL;
    if (uold) {
        long rc = sys_getitimer(which, uold);
        if (rc) return rc;
    }
    if (!user_range_ok(unew, sizeof(struct abi_itimerval))) return -EFAULT;
    const struct abi_itimerval *n = (const void *)unew;
    uint64_t value = tv_to_ticks(&n->it_value);
    t->alarm_interval = tv_to_ticks(&n->it_interval);
    t->alarm_at = value ? timer_ticks() + (value ? value : 1) : 0;
    return 0;
}
