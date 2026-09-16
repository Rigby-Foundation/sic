/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "proc/syscall.h"
#include "arch/x86_64/idt.h"

#define NSIG 64

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28

#define SIG_DFL 0UL
#define SIG_IGN 1UL

#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTART   0x10000000

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* musl's struct k_sigaction for x86_64 (what rt_sigaction takes). */
struct k_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint32_t mask[2];
} __attribute__((packed));

struct task;

/* Signal dispositions, shared by the threads of a process (CLONE_SIGHAND). */
struct sighand {
    struct k_sigaction actions[NSIG];
    int refs;
};

/* Register set common to the syscall and interrupt frames. */
struct sigregs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rbx, rdi, rsi, rdx, rcx, rax;
    uint64_t rip, rflags, rsp;
};

#ifdef CONFIG_SIGNALS
int  signal_init_task(struct task *t);            /* fresh sighand; 0 or -ENOMEM */
int  signal_fork(struct task *child, struct task *parent);     /* copy of the dispositions */
void signal_clone(struct task *child, struct task *parent);    /* thread: shared dispositions */
void signal_exec(struct task *t);                 /* handlers back to default */
void signal_release(struct task *t);              /* drop the sighand reference */

int  task_send_signal(struct task *t, int sig);   /* 0 or -errno */
int  task_signal_pending(struct task *t);

/* Deliver pending signals before returning to user mode. */
void signal_deliver_syscall(struct syscall_frame *f);
void signal_deliver_irq(struct interrupt_frame *f);
/* A fault in user mode: queue `sig` with the faulting address and deliver
 * (terminates the task if there's no handler). */
int  signal_fault(struct interrupt_frame *f, int sig, uint64_t addr);   /* 1 = handler set up */

/* System calls (see proc/syscall.c for argument decoding). */
long sys_rt_sigaction(int sig, uint64_t uact, uint64_t uoact, uint64_t setsize);
long sys_rt_sigprocmask(int how, uint64_t uset, uint64_t uoset, uint64_t setsize);
long sys_rt_sigpending(uint64_t uset, uint64_t setsize);
long sys_rt_sigsuspend(uint64_t uset, uint64_t setsize);
long sys_rt_sigreturn(struct syscall_frame *f);
long sys_kill(long pid, int sig);
long sys_tkill(long tid, int sig);
long sys_pause(void);
long sys_alarm(uint64_t secs);
long sys_setitimer(int which, uint64_t unew, uint64_t uold);
long sys_getitimer(int which, uint64_t uold);
#else
static inline int  signal_init_task(struct task *t) { (void)t; return 0; }
static inline int  signal_fork(struct task *c, struct task *p) { (void)c; (void)p; return 0; }
static inline void signal_clone(struct task *c, struct task *p) { (void)c; (void)p; }
static inline void signal_exec(struct task *t) { (void)t; }
static inline void signal_release(struct task *t) { (void)t; }
static inline int  task_send_signal(struct task *t, int sig) { (void)t; (void)sig; return -38; /* ENOSYS */ }
static inline int  task_signal_pending(struct task *t) { (void)t; return 0; }
static inline void signal_deliver_syscall(struct syscall_frame *f) { (void)f; }
static inline void signal_deliver_irq(struct interrupt_frame *f) { (void)f; }
#endif
