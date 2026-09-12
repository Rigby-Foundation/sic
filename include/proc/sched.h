/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "fs/vfs.h"

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_ZOMBIE };

struct cpu;

struct task {
    uint64_t rsp;               /* saved stack pointer while not running (must be first) */
    uint64_t kstack_top;        /* top of the kernel stack (TSS.rsp0 / syscall entry) */
    uint64_t pml4;              /* address space; kernel tasks use the kernel PML4 */
    uint32_t id;
    enum task_state state;
    char     name[24];
    void    *stack;             /* base of the kernel stack, NULL for the boot task */
    size_t   stack_pages;
    uint64_t wake_at;           /* tick to wake at while sleeping */
    uint32_t slice_left;        /* ticks left in this quantum */
    uint64_t runtime;           /* ticks spent running */
    int      is_user;
    int      exit_code;
    uint64_t user_rip, user_rsp; /* initial user context for processes */
    uint32_t parent_id;
    int      killed_sig;        /* nonzero if terminated by a fault */
    uint64_t fs_base;           /* user TLS pointer (arch_prctl) */
    uint64_t brk_start, brk_end;/* program break */
    uint64_t mmap_next;         /* bump pointer for anonymous mmap */
    uint8_t  fpu[512] __attribute__((aligned(16)));
    int      wake_pending;      /* task_wake() raced ahead of task_block() */
    int      reaped;            /* zombie collected by waitpid; idle may free it */
    struct file  *files[MAX_FDS];
    struct vnode *cwd;
    uint32_t last_cpu;
    struct task *next;          /* all-tasks list */
    struct task *rq_next;       /* run queue */
};

typedef void (*task_entry_t)(void *arg);

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

void task_yield(void);
void task_sleep_ms(uint64_t ms);
void task_exit(void) __attribute__((noreturn));
void task_exit_code(int code) __attribute__((noreturn));
void task_join(uint32_t id);            /* wait until task `id` no longer exists */
int  task_alive(uint32_t id);
/* Reap a zombie child: pid > 0 for a specific child, -1 for any. Returns the
 * child's id and stores its exit code, 0 if children exist but none exited
 * yet, -1 if there are no children. */
long task_collect_child(long pid, int *status);
struct task *task_find(uint32_t id);    /* not locked: only for read-mostly use */

/* Block/wake: block puts the current task to sleep until task_wake() on it. */
void task_block(void);
void task_wake(struct task *t);

/* Called from the timer interrupt and the IRQ epilogue respectively. */
void sched_tick(void);
void sched_preempt(void);

/* Called by new tasks from task_trampoline to drop the scheduler lock. */
void sched_unlock_new_task(void);

void sched_dump(void);
