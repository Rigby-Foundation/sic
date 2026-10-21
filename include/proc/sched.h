/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "fs/vfs.h"
#include "proc/signal.h"
#include "proc/mm.h"
#include "fs/fdtable.h"
#include "mm/vmm.h"
#include "asm/task.h"

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_STOPPED, TASK_ZOMBIE };

struct cpu;

struct task {
    struct arch_task arch;      /* register/stack state; must be first (see asm/task.h) */
    uint64_t kstack_top;        /* top of the kernel stack (TSS.rsp0 / syscall entry) */
    pgd_t    pgd;               /* address space to run with: mm->pgd, or the kernel's */
    struct mm *mm;              /* user address space (NULL for kernel tasks) */
    uint32_t id;
    enum task_state state;
    char     name[24];
    void    *stack;             /* base of the kernel stack, NULL for the boot task */
    size_t   stack_pages;
    uint64_t wake_at;           /* tick to wake at while sleeping */
    uint32_t slice_left;        /* ticks left in this quantum */
    uint64_t runtime;           /* ticks spent running */
    uint64_t utime, stime;      /* of those: ticks in user mode, in the kernel (/proc) */
    uint64_t start_tick;        /* when it was started (/proc/<pid>/stat) */
    int      is_user;
    int      exit_code;
    uint64_t user_rip, user_rsp; /* initial user context for processes */
    uint32_t parent_id;
    uint32_t tgid;              /* thread group = process id (the leader's id) */
    int      is_thread;         /* not the group leader: reaped without wait4 */
    uint64_t clear_child_tid;   /* CLONE_CHILD_CLEARTID / set_tid_address */
    volatile int group_exit;    /* the process is exiting: die at the next delivery point */
    int      killed_sig;        /* nonzero if terminated by a fault */
    int      stop_sig;          /* signal that stopped the task */
    int      stop_reported;     /* stopped state was reported to parent */
    /* the last TASK_TRACE system calls (number, first argument, result): printed when a fault kills the task */
#define TASK_TRACE 16
    struct { uint16_t nr; unsigned long a1; long ret; } trace[TASK_TRACE];
    unsigned trace_i;
    int      sys_nr;            /* the system call it is in now, -1 if none (sched_dump) */
    unsigned long sys_a1;
    uint64_t sig_pending, sig_blocked;
    uint64_t sig_saved_mask;    /* mask to restore after a sigsuspend handler */
    int      sig_saved_mask_valid;
    struct sighand *sighand;
    uint64_t alarm_at;          /* tick for SIGALRM, 0 = none */
    uint64_t alarm_interval;    /* ticks; re-arm after firing (setitimer) */
    int      wake_pending;      /* task_wake() raced ahead of task_block() */
    int      reaped;            /* zombie collected by waitpid; idle may free it */
    struct fdtable *fdt;
    struct vnode *cwd;
    uint32_t last_cpu;
    struct task *next;          /* all-tasks list */
    struct task *rq_next;       /* run queue */
};


void         sched_init(void);          /* turn the boot context into task 0 + create idle */
void         sched_init_ap(struct cpu *c);
void         sched_enter_idle(void) __attribute__((noreturn));
struct task *task_create(const char *name, task_entry_t entry, void *arg);
/* Two-step creation for callers that need to fill in more fields first. */
struct task *task_alloc(const char *name, task_entry_t entry, void *arg);
/* Same, but leave `reserve` bytes untouched at the top of the kernel stack
 * (fork places the child's return frame there). */
struct task *task_alloc_reserve(const char *name, task_entry_t entry, void *arg, size_t reserve);
void         task_start(struct task *t);
struct task *task_current(void);
int          sched_started(void);       /* tasks may sleep (past sched_init) */
int          arch_task_backtrace(const struct task *t, uint64_t *pcs, int max);   /* a task that is switched out */
void         sched_dump_request(void);

/* Accounting and introspection for /proc and sysinfo(). */
struct vnode;
void         sched_account(int user_mode);  /* every timer tick, on every CPU */
struct proc_info {
    uint32_t pid, ppid;                     /* process (thread group) and parent */
    char     name[24];
    char     state;                         /* R S D T Z, as Linux reports it */
    uint64_t utime, stime, start_tick;      /* summed over the process's threads */
    uint32_t threads;
};
int          sched_proc_list(struct proc_info *out, int max);   /* processes, newest first */
int          sched_proc_get(uint32_t pid, struct proc_info *out); /* 0, or -1 if gone */
struct mm   *sched_proc_mm(uint32_t pid);   /* the process's mm, referenced (mm_put it), or NULL */
int          sched_vnode_is_cwd(const struct vnode *n);           /* some task's working directory */
struct cpu_times { uint64_t user, system, idle; };              /* ticks */
void         sched_cpu_times(struct cpu_times *out, int ncpu);
void         sched_loadavg(unsigned long avg[3]);              /* fixed point, 11 fraction bits */
int          sched_nr_running(void);
uint32_t     sched_last_pid(void);
uint64_t     sched_forks(void);  /* sched_dump with call chains, from the idle loop (any context) */

void task_yield(void);
void task_sleep_ms(uint64_t ms);
void task_exit(void) __attribute__((noreturn));
void task_exit_code(int code) __attribute__((noreturn));
/* Exit the whole thread group: other threads die at their next return to user mode. */
void task_exit_group(int code) __attribute__((noreturn));
void task_kill_other_threads(void);     /* exec from a multi-threaded process */
void task_join(uint32_t id);            /* wait until task `id` no longer exists */
int  task_alive(uint32_t id);
/* Reap a zombie child: pid > 0 for a specific child, -1 for any. Returns the
 * child's id and stores its exit code, 0 if children exist but none exited
 * yet, -1 if there are no children. */
long task_collect_child(long pid, int *status);
long task_collect_child_opts(long pid, int *status, int options);
void task_reap_zombies(void);
struct task *task_find(uint32_t id);    /* not locked: only for read-mostly use */
/* Call fn for every task (under the scheduler lock; fn must not block). */
void task_foreach(void (*fn)(struct task *t, void *arg), void *arg);

/* Block/wake: block puts the current task to sleep until task_wake() on it. */
void task_block(void);
void task_wake(struct task *t);
void task_continue(struct task *t);
void task_stop(struct task *t, int sig);

/* Called from the timer interrupt and the IRQ epilogue respectively. */
void sched_tick(void);
void sched_preempt(void);

/* Called by new tasks from task_trampoline to drop the scheduler lock. */
void sched_unlock_new_task(void);

void sched_dump(void);
