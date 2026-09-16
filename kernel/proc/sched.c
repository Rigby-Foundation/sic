/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Preemptive round-robin scheduler for kernel and user tasks, SMP-safe with
 * one global run queue protected by sched_lock. */
#include "proc/sched.h"
#include "arch/x86_64/cpu.h"
#include "mm/heap.h"
#include "arch/x86_64/timer.h"
#include "mm/vmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "proc/futex.h"

#define STACK_PAGES 16      /* 64 KiB: filesystem code keeps 4 KiB blocks on the stack */
#define TIME_SLICE  (TIMER_HZ / 100)     /* 10 ms */

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void task_trampoline(void);
extern char stack_top[];                 /* boot stack, entry.S */

static struct task boot_task = { .id = 0, .tgid = 0, .name = "main", .state = TASK_RUNNING };
static struct task *all_tasks = &boot_task;
static struct task *rq_head, *rq_tail;
static uint32_t next_id = 1;
static int started;
static spinlock_t sched_lock = SPINLOCK_INIT;

#define CUR (this_cpu()->current)

/* ---- run queue (sched_lock held) --------------------------------------------- */

static void rq_push(struct task *t)
{
    t->state = TASK_READY;
    t->rq_next = NULL;
    if (rq_tail) rq_tail->rq_next = t; else rq_head = t;
    rq_tail = t;
}

static struct task *rq_pop(void)
{
    struct task *t = rq_head;
    if (t) {
        rq_head = t->rq_next;
        if (!rq_head)
            rq_tail = NULL;
        t->rq_next = NULL;
    }
    return t;
}

static int is_idle(struct task *t)
{
    for (uint32_t i = 0; i < cpu_count; i++)
        if (cpus[i].idle == t)
            return 1;
    return 0;
}

/* ---- reaping (called from idle with interrupts on, lock not held) ------------------- */

static void reap_zombies(void)
{
    struct task *dead = NULL;

    uint64_t f = spin_lock_irqsave(&sched_lock);
    struct task **pp = &all_tasks;
    while (*pp) {
        struct task *t = *pp;
        int busy = 0;
        for (uint32_t i = 0; i < cpu_count; i++)
            if (cpus[i].current == t)
                busy = 1;
        /* User zombies wait to be collected by their parent (waitpid, or
         * task_collect_child from the kernel), unless the parent is gone.
         * Threads are not waitable and go straight away. */
        int waiting_parent = 0;
        if (t->state == TASK_ZOMBIE && t->is_user && !t->is_thread && !t->reaped)
            for (struct task *p = all_tasks; p; p = p->next)
                if (p->id == t->parent_id && p->state != TASK_ZOMBIE)
                    waiting_parent = 1;
        if (t->state == TASK_ZOMBIE && !busy && !waiting_parent) {
            *pp = t->next;
            t->next = dead;
            dead = t;
            continue;
        }
        pp = &t->next;
    }
    spin_unlock_irqrestore(&sched_lock, f);

    /* Freeing may need TLB shootdowns, so do it outside the lock. */
    while (dead) {
        struct task *t = dead;
        dead = t->next;
        fdt_put(t->fdt);
        signal_release(t);
        mm_put(t->mm);
        if (t->stack)
            heap_free_pages(t->stack, t->stack_pages);
        kfree(t);
    }
}

static void wake_sleepers(uint64_t now)
{
    for (struct task *t = all_tasks; t; t = t->next) {
        if (t->state == TASK_SLEEPING && t->wake_at <= now)
            rq_push(t);
#ifdef CONFIG_SIGNALS
        if (t->alarm_at && t->alarm_at <= now && t->state != TASK_ZOMBIE) {
            t->alarm_at = t->alarm_interval ? now + t->alarm_interval : 0;
            __atomic_fetch_or(&t->sig_pending, 1UL << (SIGALRM - 1), __ATOMIC_SEQ_CST);
            if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                rq_push(t);
            else
                t->wake_pending = 1;
        }
#endif
    }
}

/* ---- core switch (sched_lock held, interrupts off) ---------------------------------- */

static void schedule(void)
{
    struct cpu *cpu = this_cpu();
    struct task *prev = cpu->current;
    struct task *next = rq_pop();

    if (!next) {
        if (prev->state == TASK_RUNNING)
            return;                         /* nothing else to do, keep going */
        next = cpu->idle;
    }
    if (prev->state == TASK_RUNNING) {
        if (is_idle(prev))
            prev->state = TASK_READY;       /* idle never sits in the queue */
        else
            rq_push(prev);                  /* preempted or yielded: back of the queue */
    }

    next->state = TASK_RUNNING;
    next->slice_left = TIME_SLICE;
    next->last_cpu = cpu->index;
    if (next == prev)
        return;

    cpu->current = next;
    cpu_set_kernel_stack(next->kstack_top);
    if (prev->is_user)
        fpu_save(prev->fpu);
    if (next->is_user) {
        fpu_restore(next->fpu);
        wrmsr(MSR_FS_BASE, next->fs_base);
    }
    if (next->pml4 != read_cr3())
        write_cr3(next->pml4);
    switch_context(&prev->rsp, next->rsp);
    /* Back on `prev`'s stack, some time later — possibly on another CPU. */
}

void sched_unlock_new_task(void)
{
    spin_unlock(&sched_lock);
}

/* ---- tasks ------------------------------------------------------------------------ */

static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        reap_zombies();
        __asm__ volatile("sti; hlt");
    }
}

struct task *task_alloc(const char *name, task_entry_t entry, void *arg)
{
    return task_alloc_reserve(name, entry, arg, 0);
}

struct task *task_alloc_reserve(const char *name, task_entry_t entry, void *arg, size_t reserve)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->stack_pages = STACK_PAGES;
    t->stack = heap_alloc_pages(STACK_PAGES);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }
    t->kstack_top = (uint64_t)t->stack + STACK_PAGES * 4096;
    t->pml4 = vmm_kernel_pml4();
    memcpy(t->fpu, fpu_initial_state, sizeof(t->fpu));
    if (signal_init_task(t) != 0) {
        heap_free_pages(t->stack, STACK_PAGES);
        kfree(t);
        return NULL;
    }

    size_t n = strlen(name);
    if (n >= sizeof(t->name))
        n = sizeof(t->name) - 1;
    memcpy(t->name, name, n);

    /* Initial frame, as switch_context expects to pop it:
     * r15 r14 r13(entry) r12(arg) rbx rbp | return -> task_trampoline. */
    uint64_t *sp = (uint64_t *)(t->kstack_top - ((reserve + 15) & ~15UL));
    *--sp = (uint64_t)task_trampoline;
    *--sp = 0;                      /* rbp */
    *--sp = 0;                      /* rbx */
    *--sp = (uint64_t)arg;          /* r12 */
    *--sp = (uint64_t)entry;        /* r13 */
    *--sp = 0;                      /* r14 */
    *--sp = 0;                      /* r15 */
    t->rsp = (uint64_t)sp;
    t->state = TASK_READY;
    return t;
}

void task_start(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->id = next_id++;
    if (!t->is_thread)
        t->tgid = t->id;
    t->next = all_tasks;
    all_tasks = t;
    rq_push(t);
    spin_unlock_irqrestore(&sched_lock, f);
}

struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    struct task *t = task_alloc(name, entry, arg);
    if (t)
        task_start(t);
    return t;
}

static struct task *make_idle(struct cpu *c)
{
    char name[24] = "idle/";
    name[5] = '0' + (char)(c->index % 10);
    struct task *t = task_alloc(name, idle_main, NULL);
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->id = next_id++;
    t->next = all_tasks;
    all_tasks = t;
    spin_unlock_irqrestore(&sched_lock, f);
    return t;
}

void sched_init(void)
{
    struct cpu *c = this_cpu();
    boot_task.slice_left = TIME_SLICE;
    boot_task.kstack_top = (uint64_t)stack_top;
    boot_task.pml4 = vmm_kernel_pml4();
    c->current = &boot_task;
    c->idle = make_idle(c);
    started = 1;
    kprintf("sched: %u ms time slice, idle task %u\n", TIME_SLICE * 1000 / TIMER_HZ, c->idle->id);
}

void sched_init_ap(struct cpu *c)
{
    c->idle = make_idle(c);
    c->current = c->idle;
    c->idle->state = TASK_RUNNING;
    cpu_set_kernel_stack(c->idle->kstack_top);
}

/* Switch an AP from its bootstrap stack onto its idle task for good. */
void sched_enter_idle(void)
{
    struct cpu *c = this_cpu();
    uint64_t scratch;
    spin_lock_irqsave(&sched_lock);
    switch_context(&scratch, c->idle->rsp);
    __builtin_unreachable();
}

struct task *task_current(void) { return CUR; }

void task_yield(void)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    schedule();
    spin_unlock_irqrestore(&sched_lock, f);
}

void task_sleep_ms(uint64_t ms)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    if (CUR->wake_pending) {                /* a wake raced ahead of us: don't sleep */
        CUR->wake_pending = 0;
        spin_unlock_irqrestore(&sched_lock, f);
        return;
    }
    CUR->wake_at = timer_ticks() + (ms * TIMER_HZ + 999) / 1000;
    CUR->state = TASK_SLEEPING;
    schedule();
    spin_unlock_irqrestore(&sched_lock, f);
}

void task_block(void)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    if (CUR->wake_pending) {
        CUR->wake_pending = 0;
    } else {
        CUR->state = TASK_BLOCKED;
        schedule();
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void task_wake(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    if (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING)
        rq_push(t);
    else if (t->state != TASK_ZOMBIE)
        t->wake_pending = 1;
    spin_unlock_irqrestore(&sched_lock, f);
}

struct task *task_find(uint32_t id)
{
    for (struct task *t = all_tasks; t; t = t->next)
        if (t->id == id)
            return t;
    return NULL;
}

void task_foreach(void (*fn)(struct task *t, void *arg), void *arg)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *t = all_tasks; t; t = t->next)
        fn(t, arg);
    spin_unlock_irqrestore(&sched_lock, f);
}

long task_collect_child(long pid, int *status)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    long ret = -1;
    for (struct task *t = all_tasks; t; t = t->next) {
        if (t->parent_id != CUR->tgid || t->reaped || t == CUR || t->is_thread)
            continue;
        if (pid > 0 && t->id != (uint32_t)pid)
            continue;
        if (t->state == TASK_ZOMBIE) {
            *status = t->exit_code;
            t->reaped = 1;
            ret = t->id;
            break;
        }
        ret = 0;                        /* a live child exists */
    }
    spin_unlock_irqrestore(&sched_lock, f);
    return ret;
}

/* CLONE_CHILD_CLEARTID: tell a joiner we're gone. */
static void clear_child_tid(struct task *t)
{
    if (!t->clear_child_tid || !t->mm)
        return;
    uint64_t phys = vmm_translate_in(t->mm->pml4, t->clear_child_tid);
    if (phys) {
        *(volatile uint32_t *)P2V(phys) = 0;
        futex_wake_phys(phys, 1);
    }
}

void task_exit_code(int code)
{
    struct task *t = CUR;
    if (t->is_user && !t->is_thread && !t->group_exit)
        task_exit_group(code);              /* the leader leaving ends the process */
    clear_child_tid(t);
    spin_lock_irqsave(&sched_lock);
    t->exit_code = code;
    CUR->state = TASK_ZOMBIE;
    schedule();
    kprintf("sched: zombie %s resumed?!\n", CUR->name);
    for (;;) __asm__ volatile("cli; hlt");
}

void task_exit(void) { task_exit_code(0); }

static void group_exit_cb(struct task *t, void *arg)
{
    struct task *self = arg;
    if (t == self || t->tgid != self->tgid || t->state == TASK_ZOMBIE || t->group_exit)
        return;
    t->exit_code = self->exit_code;
    t->group_exit = 1;
    if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
        rq_push(t);                         /* interrupt whatever it's waiting on */
    else
        t->wake_pending = 1;
}

/* exec: everyone else in the group must go, but we carry on. */
void task_kill_other_threads(void)
{
    struct task *self = CUR;
    task_foreach(group_exit_cb, self);
}

void task_exit_group(int code)
{
    struct task *self = CUR;
    self->exit_code = code;
    self->group_exit = 1;
    task_foreach(group_exit_cb, self);
#ifdef CONFIG_SIGNALS
    if (self->is_user) {
        struct task *parent = task_find(self->parent_id);
        if (parent && parent->is_user && parent->state != TASK_ZOMBIE)
            task_send_signal(parent, SIGCHLD);
    }
#endif
    /* The leader's zombie is what the parent waits for; a thread that called
     * exit_group ends itself here, and the leader dies at its next delivery point. */
    clear_child_tid(self);
    spin_lock_irqsave(&sched_lock);
    self->state = TASK_ZOMBIE;
    schedule();
    kprintf("sched: zombie %s resumed?!\n", CUR->name);
    for (;;) __asm__ volatile("cli; hlt");
}

int task_alive(uint32_t id)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int alive = 0;
    for (struct task *t = all_tasks; t; t = t->next)
        if (t->id == id && t->state != TASK_ZOMBIE) {
            alive = 1;
            break;
        }
    spin_unlock_irqrestore(&sched_lock, f);
    return alive;
}

void task_join(uint32_t id)
{
    while (task_alive(id))
        task_sleep_ms(1);
}

/* Timer tick on every CPU: account runtime, wake sleepers, expire the quantum.
 * Runs with interrupts off inside the IRQ handler; the actual switch happens
 * in sched_preempt() once the controller has been acknowledged. */
void sched_tick(void)
{
    if (!started)
        return;
    spin_lock(&sched_lock);
    struct task *t = CUR;
    t->runtime++;
    wake_sleepers(timer_ticks());
    if (t->slice_left > 0)
        t->slice_left--;
    if (rq_head && (t->slice_left == 0 || is_idle(t)))
        t->slice_left = 0;          /* sched_preempt() will switch */
    spin_unlock(&sched_lock);
}

void sched_preempt(void)
{
    if (!started || !this_cpu()->current)
        return;
    spin_lock(&sched_lock);
    if (rq_head && (CUR->slice_left == 0 || is_idle(CUR)))
        schedule();
    spin_unlock(&sched_lock);
}

void sched_dump(void)
{
    static const char *const names[] = { "ready", "running", "sleeping", "blocked", "zombie" };
    uint64_t f = spin_lock_irqsave(&sched_lock);
    kprintf("tasks:\n");
    for (struct task *t = all_tasks; t; t = t->next)
        kprintf("  %2u %-12s %-8s cpu %u  runtime %lu ms%s\n",
                t->id, t->name, names[t->state], t->last_cpu,
                t->runtime * 1000 / TIMER_HZ, t->is_user ? "  [user]" : "");
    spin_unlock_irqrestore(&sched_lock, f);
}
