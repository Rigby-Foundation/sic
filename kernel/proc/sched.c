/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Preemptive round-robin scheduler for kernel and user tasks, SMP-safe with
 * one global run queue protected by sched_lock. */
#include "proc/sched.h"
#include "asm/cpu.h"
#include "mm/heap.h"
#include "proc/mm.h"
#ifdef CONFIG_NET
#include "net/net.h"
#endif
#include "asm/timer.h"
#include "mm/vmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "proc/futex.h"
#include "asm/arch.h"
#ifdef CONFIG_B44
#include "drivers/b44.h"
#endif

#define STACK_PAGES 16      /* 64 KiB: filesystem code keeps 4 KiB blocks on the stack */
#define TIME_SLICE  (TIMER_HZ / 100)     /* 10 ms */

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

/* ---- reaping ------------------------------------------------------------------------ */

void task_reap_zombies(void)
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
            kstack_free(t->stack, t->stack_pages);
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
            __atomic_fetch_or(&t->sig_pending, 1ULL << (SIGALRM - 1), __ATOMIC_SEQ_CST);
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
    arch_switch(prev, next);
    /* Back on `prev`'s stack, some time later — possibly on another CPU. */
}

void sched_unlock_new_task(void)
{
    spin_unlock(&sched_lock);
}

/* ---- tasks ------------------------------------------------------------------------ */

/* Scroll Lock: what is everyone doing? The dump runs from the idle loop,
 * which keeps running when every task is stuck waiting. */
static volatile int dump_requested;

/* The idle task is switched out only while it waits for an interrupt.
 * Anywhere else in its loop it may hold a lock (reaping frees memory under
 * the heap's), and it never sits in the run queue: it only comes back when
 * its CPU has nothing else to run, which is never while that CPU spins on
 * the lock the idle task holds. */
static volatile int idle_waiting[MAX_CPUS];

static void idle_main(void *arg)
{
    (void)arg;
    uint32_t me = this_cpu()->index;        /* an idle task never leaves its CPU */
    for (;;) {
        task_reap_zombies();
        if (dump_requested && __atomic_exchange_n(&dump_requested, 0, __ATOMIC_SEQ_CST)) {
            sched_dump();
            heap_dump();
#ifdef CONFIG_NET
            tcp_dump();
#endif
#ifdef CONFIG_B44
            b44_dump();
#endif
        }
        if (rq_head) {                      /* work came in meanwhile: no need to wait for a tick */
            task_yield();
            continue;
        }
        idle_waiting[me] = 1;
        arch_idle();
        idle_waiting[me] = 0;
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
    t->stack = kstack_alloc(STACK_PAGES);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }
    t->kstack_top = (uint64_t)(uintptr_t)t->stack + STACK_PAGES * 4096;
    t->sys_nr = -1;
    t->pgd = vmm_kernel_pgd();
    if (signal_init_task(t) != 0) {
        kstack_free(t->stack, STACK_PAGES);
        kfree(t);
        return NULL;
    }

    size_t n = strlen(name);
    if (n >= sizeof(t->name))
        n = sizeof(t->name) - 1;
    memcpy(t->name, name, n);

    arch_task_setup(t, entry, arg, reserve);
    t->state = TASK_READY;
    return t;
}

static uint64_t forks;                       /* tasks ever started (/proc/stat "processes") */

void task_start(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->id = next_id++;
    t->start_tick = timer_ticks();
    forks++;
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
    boot_task.kstack_top = (uint64_t)(uintptr_t)stack_top;
    boot_task.pgd = vmm_kernel_pgd();
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
    spin_lock_irqsave(&sched_lock);
    arch_switch_first(c->idle);
}

/* The CPU, then its current task: with interrupts on, a preemption between
 * the two loads can move us to another CPU and answer with its task. */
struct task *task_current(void)
{
    unsigned long f = irq_save();
    struct task *t = CUR;
    irq_restore(f);
    return t;
}
int sched_started(void) { return started; }

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

/* A task became runnable: an idle CPU is kicked out of its halt so it
 * picks the task up now instead of at its next tick. */
static void wake_hint(struct task *t)
{
    (void)t;
    for (uint32_t i = 0; i < cpu_count; i++)
        if (cpus[i].online && cpus[i].current == cpus[i].idle && &cpus[i] != this_cpu()) {
            arch_wake_cpu(&cpus[i]);
            return;
        }
}

void task_wake(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    if (t->state == TASK_STOPPED) {
        spin_unlock_irqrestore(&sched_lock, f);
        return;
    }
    if (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING) {
        rq_push(t);
        wake_hint(t);
    } else if (t->state != TASK_ZOMBIE) {
        t->wake_pending = 1;
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void task_continue(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    if (t->state == TASK_STOPPED) {
        t->stop_sig = 0;
        t->stop_reported = 0;
        rq_push(t);
        wake_hint(t);
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void task_stop(struct task *t, int sig)
{
    t->stop_sig = sig;
    t->stop_reported = 0;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->state = TASK_STOPPED;
    spin_unlock_irqrestore(&sched_lock, f);
    struct task *parent = task_find(t->parent_id);
    if (parent) task_wake(parent);
    task_yield();
}

/* The list walk must be locked: the reaper on another CPU unlinks zombies
 * and reuses their `next` for its own list, which a bare walk would follow
 * off the end (seen as a spurious ESRCH from tkill). */
struct task *task_find(uint32_t id)
{
    struct task *found = NULL;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *t = all_tasks; t; t = t->next)
        if (t->id == id) { found = t; break; }
    spin_unlock_irqrestore(&sched_lock, f);
    return found;
}

void task_foreach(void (*fn)(struct task *t, void *arg), void *arg)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *t = all_tasks; t; t = t->next)
        fn(t, arg);
    spin_unlock_irqrestore(&sched_lock, f);
}

long task_collect_child_opts(long pid, int *status, int options)
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
        if ((options & WUNTRACED) && t->state == TASK_STOPPED && !t->stop_reported) {
            t->stop_reported = 1;
            *status = (t->stop_sig << 8) | 0x7f;
            ret = t->id;
            break;
        }
        ret = 0;                        /* a live child exists */
    }
    spin_unlock_irqrestore(&sched_lock, f);
    if (ret > 0)
        task_reap_zombies();
    return ret;
}

long task_collect_child(long pid, int *status)
{
    return task_collect_child_opts(pid, status, 0);
}

/* CLONE_CHILD_CLEARTID: tell a joiner we're gone. */
static void clear_child_tid(struct task *t)
{
    if (!t->clear_child_tid || !t->mm)
        return;
    uint64_t phys = vmm_translate_in(t->mm->pgd, t->clear_child_tid);
    if (phys) {
        *(volatile uint32_t *)P2V(phys) = 0;
        futex_wake_phys(phys, 1);
    }
}

/* Drop the file table and address space now rather than when the zombie is
 * reaped: pipes/sockets see EOF immediately and user physical memory is freed. */
static void release_files(struct task *t)
{
    struct fdtable *fdt = t->fdt;
    t->fdt = NULL;
    fdt_put(fdt);
}

static void release_mm(struct task *t)
{
    struct mm *mm = t->mm;
    if (!mm)
        return;
    t->mm = NULL;
    t->pgd = vmm_kernel_pgd();
    vmm_activate(t->pgd);
    mm_put(mm);
}

void task_exit_code(int code)
{
    struct task *t = CUR;
    if (t->is_user && !t->is_thread && !t->group_exit)
        task_exit_group(code);              /* the leader leaving ends the process */
    clear_child_tid(t);
    release_files(t);
    release_mm(t);
    spin_lock_irqsave(&sched_lock);
    t->exit_code = code;
    CUR->state = TASK_ZOMBIE;
    schedule();
    kprintf("sched: zombie %s resumed?!\n", CUR->name);
    arch_halt_forever();
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
    /* Reparent children whose parent died: since the parent is dead and can never wait on them,
     * mark dead children as reaped so task_reap_zombies can reclaim them. */
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *c = all_tasks; c; c = c->next) {
        if (c->parent_id == self->tgid) {
            c->parent_id = 1;
            if (c->state == TASK_ZOMBIE)
                c->reaped = 1;
        }
    }
    spin_unlock_irqrestore(&sched_lock, f);

    /* The leader's zombie is what the parent waits for; a thread that called
     * exit_group ends itself here, and the leader dies at its next delivery point. */
    clear_child_tid(self);
    release_files(self);
    release_mm(self);
    spin_lock_irqsave(&sched_lock);
    self->state = TASK_ZOMBIE;
    schedule();
    kprintf("sched: zombie %s resumed?!\n", CUR->name);
    arch_halt_forever();
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
    struct task *t = CUR;
    if (rq_head && (is_idle(t) ? idle_waiting[this_cpu()->index] : t->slice_left == 0))
        schedule();
    spin_unlock(&sched_lock);
}

/* Where a task that is not running sits in the kernel: return addresses
 * from its saved context (arch_task_backtrace), with symbols. */
static void dump_chain(struct task *t)
{
    uint64_t pcs[8];
    int n = arch_task_backtrace(t, pcs, 8);
    for (int i = 0; i < n; i++) {
        uint64_t off;
        const char *s = ksym_name(pcs[i], &off);
        kprintf(" %s+%llx", s ? s : "?", off);
    }
}

void sched_dump(void)
{
    static const char *const names[] = { "ready", "running", "sleeping", "blocked", "zombie" };
    uint64_t f = spin_lock_irqsave(&sched_lock);
    kprintf("tasks:\n");
    for (struct task *t = all_tasks; t; t = t->next) {
        kprintf("  %2u %-12s %-8s cpu %u  runtime %llu ms%s", t->id, t->name, names[t->state], t->last_cpu,
                t->runtime * 1000 / TIMER_HZ, t->is_user ? "  [user]" : "");
        if (t->is_user && t->sys_nr >= 0) kprintf("  in syscall %d(%lx)", t->sys_nr, t->sys_a1);
        if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED || t->state == TASK_READY) { kprintf("  at"); dump_chain(t); }
        kprintf("\n");
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void sched_dump_request(void) { dump_requested = 1; }


/* ---- accounting and introspection (/proc, sysinfo) ------------------------- */

static uint64_t cpu_user[MAX_CPUS], cpu_sys[MAX_CPUS], cpu_idle[MAX_CPUS];

/* Load average as Linux keeps it: exponentially decayed counts of runnable
 * tasks, sampled every 5 s, fixed point with 11 fraction bits. */
#define FSHIFT  11
#define FIXED_1 (1UL << FSHIFT)
static unsigned long loadavg[3];
static const unsigned long load_exp[3] = { 1884, 2014, 2037 };   /* 1, 5, 15 minutes */

/* Timer tick on every CPU, with whether it interrupted user mode. */
void sched_account(int user_mode)
{
    if (!started)
        return;
    struct cpu *c = this_cpu();
    uint32_t i = c->index < MAX_CPUS ? c->index : 0;
    spin_lock(&sched_lock);
    struct task *t = CUR;
    if (is_idle(t))
        cpu_idle[i]++;
    else if (user_mode) {
        cpu_user[i]++;
        t->utime++;
    } else {
        cpu_sys[i]++;
        t->stime++;
    }
    if (i == 0 && timer_ticks() % (5 * TIMER_HZ) == 0) {
        unsigned long n = 0;
        for (struct task *u = all_tasks; u; u = u->next)
            if ((u->state == TASK_RUNNING || u->state == TASK_READY) && !is_idle(u) && u->is_user)
                n++;
        for (int k = 0; k < 3; k++)
            loadavg[k] = (loadavg[k] * load_exp[k] + n * FIXED_1 * (FIXED_1 - load_exp[k])) >> FSHIFT;
    }
    spin_unlock(&sched_lock);
}

static char state_char(const struct task *t)
{
    switch (t->state) {
    case TASK_READY: case TASK_RUNNING: return 'R';
    case TASK_SLEEPING: case TASK_BLOCKED: return 'S';
    case TASK_STOPPED: return 'T';
    case TASK_ZOMBIE: return 'Z';
    }
    return 'S';
}

/* sched_lock held: the process `leader` with its threads folded in. */
static void fill_info(const struct task *leader, struct proc_info *o)
{
    memset(o, 0, sizeof(*o));
    o->pid = leader->tgid;
    o->ppid = leader->parent_id;
    memcpy(o->name, leader->name, sizeof(o->name));
    o->name[sizeof(o->name) - 1] = 0;
    o->state = state_char(leader);
    o->start_tick = leader->start_tick;
    for (struct task *t = all_tasks; t; t = t->next) {
        if (!t->is_user || t->tgid != leader->tgid || t->reaped)
            continue;
        o->utime += t->utime;
        o->stime += t->stime;
        o->threads++;
        if (state_char(t) == 'R')
            o->state = 'R';                 /* any thread running makes the process running */
    }
}

static int is_process(const struct task *t)
{
    return t->is_user && !t->is_thread && !t->reaped && t->tgid == t->id;
}

int sched_proc_list(struct proc_info *out, int max)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int n = 0;
    for (struct task *t = all_tasks; t && n < max; t = t->next)
        if (is_process(t))
            fill_info(t, &out[n++]);
    spin_unlock_irqrestore(&sched_lock, f);
    return n;
}

int sched_proc_get(uint32_t pid, struct proc_info *out)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int rc = -1;
    for (struct task *t = all_tasks; t; t = t->next)
        if (t->id == pid && is_process(t)) {
            fill_info(t, out);
            rc = 0;
            break;
        }
    spin_unlock_irqrestore(&sched_lock, f);
    return rc;
}

struct mm *sched_proc_mm(uint32_t pid)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    struct mm *mm = NULL;
    for (struct task *t = all_tasks; t; t = t->next)
        if (t->id == pid && is_process(t) && t->mm && t->state != TASK_ZOMBIE) {
            mm = mm_get(t->mm);
            break;
        }
    spin_unlock_irqrestore(&sched_lock, f);
    return mm;
}

int sched_vnode_is_cwd(const struct vnode *n)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int used = 0;
    for (struct task *t = all_tasks; t && !used; t = t->next)
        used = t->cwd == n;
    spin_unlock_irqrestore(&sched_lock, f);
    return used;
}

void sched_cpu_times(struct cpu_times *out, int ncpu)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < ncpu && i < MAX_CPUS; i++) {
        out[i].user = cpu_user[i];
        out[i].system = cpu_sys[i];
        out[i].idle = cpu_idle[i];
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void sched_loadavg(unsigned long avg[3])
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int k = 0; k < 3; k++) avg[k] = loadavg[k];
    spin_unlock_irqrestore(&sched_lock, f);
}

int sched_nr_running(void)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int n = 0;
    for (struct task *t = all_tasks; t; t = t->next)
        if ((t->state == TASK_RUNNING || t->state == TASK_READY) && t->is_user && !is_idle(t))
            n++;
    spin_unlock_irqrestore(&sched_lock, f);
    return n;
}

uint32_t sched_last_pid(void) { return next_id ? next_id - 1 : 0; }
uint64_t sched_forks(void) { return forks; }
