/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Futexes, keyed by the physical address of the word so they work across
 * threads (same mm) and, incidentally, across processes sharing a page. */
#include "proc/futex.h"
#include "proc/sched.h"
#include "proc/signal.h"
#include "mm/vmm.h"
#include "arch/x86_64/timer.h"
#include "spinlock.h"
#include "abi/abi.h"

struct waiter {
    uint64_t key;
    struct task *task;
    int woken;
    struct waiter *next;
};

static struct waiter *waiters;
static spinlock_t futex_lock = SPINLOCK_INIT;

static uint64_t key_of(uint64_t uaddr)
{
    if (uaddr & 3 || uaddr < USER_BASE || uaddr >= USER_END)
        return 0;
    uint64_t phys = vmm_translate_in(task_current()->mm->pml4, uaddr);
    return phys;
}

static int wake_key(uint64_t key, int n)
{
    int count = 0;
    uint64_t f = spin_lock_irqsave(&futex_lock);
    for (struct waiter *w = waiters; w && count < n; w = w->next)
        if (w->key == key && !w->woken) {
            w->woken = 1;
            task_wake(w->task);
            count++;
        }
    spin_unlock_irqrestore(&futex_lock, f);
    return count;
}

int futex_wake_phys(uint64_t phys, int n) { return wake_key(phys, n); }

static long futex_wait(uint64_t uaddr, uint32_t val, uint64_t utimeout, int absolute)
{
    uint64_t key = key_of(uaddr);
    if (!key) return -EFAULT;

    uint64_t deadline = 0;
    if (utimeout) {
        if (utimeout < USER_BASE || utimeout + 16 > USER_END || !vmm_translate_in(task_current()->mm->pml4, utimeout))
            return -EFAULT;
        const struct abi_timespec *ts = (const void *)utimeout;
        if (ts->tv_sec < 0 || ts->tv_nsec < 0) return -EINVAL;
        uint64_t ticks = (uint64_t)ts->tv_sec * TIMER_HZ + (uint64_t)ts->tv_nsec * TIMER_HZ / 1000000000;
        deadline = absolute ? ticks : timer_ticks() + ticks;
        if (deadline <= timer_ticks())
            deadline = timer_ticks() + 1;
    }

    struct waiter w = { key, task_current(), 0, NULL };
    uint64_t f = spin_lock_irqsave(&futex_lock);
    if (*(volatile uint32_t *)uaddr != val) {
        spin_unlock_irqrestore(&futex_lock, f);
        return -EAGAIN;
    }
    w.next = waiters;
    waiters = &w;
    spin_unlock_irqrestore(&futex_lock, f);

    long rc = 0;
    for (;;) {
        if (w.woken) break;
        if (task_signal_pending(task_current())) { rc = -EINTR; break; }
        if (deadline) {
            uint64_t now = timer_ticks();
            if (now >= deadline) { rc = -ETIMEDOUT; break; }
            task_sleep_ms((deadline - now) * 1000 / TIMER_HZ + 1);
        } else {
            task_block();
        }
    }

    f = spin_lock_irqsave(&futex_lock);
    for (struct waiter **pp = &waiters; *pp; pp = &(*pp)->next)
        if (*pp == &w) { *pp = w.next; break; }
    if (w.woken) rc = 0;                    /* a wake that raced with the timeout wins */
    spin_unlock_irqrestore(&futex_lock, f);
    return rc;
}

static long futex_requeue(uint64_t uaddr, uint32_t nwake, uint32_t nrequeue, uint64_t uaddr2, int cmp, uint32_t val3)
{
    uint64_t k1 = key_of(uaddr), k2 = key_of(uaddr2);
    if (!k1 || !k2) return -EFAULT;
    uint64_t f = spin_lock_irqsave(&futex_lock);
    if (cmp && *(volatile uint32_t *)uaddr != val3) {
        spin_unlock_irqrestore(&futex_lock, f);
        return -EAGAIN;
    }
    int woken = 0, moved = 0;
    for (struct waiter *w = waiters; w; w = w->next) {
        if (w->key != k1 || w->woken) continue;
        if ((uint32_t)woken < nwake) { w->woken = 1; task_wake(w->task); woken++; }
        else if ((uint32_t)moved < nrequeue) { w->key = k2; moved++; }
    }
    spin_unlock_irqrestore(&futex_lock, f);
    return woken + (cmp ? moved : 0);
}

long sys_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utimeout, uint64_t uaddr2, uint32_t val3)
{
    int absolute = op & FUTEX_CLOCK_REALTIME;
    switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT:        return futex_wait(uaddr, val, utimeout, 0);
    case FUTEX_WAIT_BITSET: return futex_wait(uaddr, val, utimeout, 1 || absolute);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        uint64_t key = key_of(uaddr);
        if (!key) return -EFAULT;
        return wake_key(key, (int)val);
    }
    case FUTEX_REQUEUE:     return futex_requeue(uaddr, val, (uint32_t)utimeout, uaddr2, 0, 0);
    case FUTEX_CMP_REQUEUE: return futex_requeue(uaddr, val, (uint32_t)utimeout, uaddr2, 1, val3);
    }
    return -ENOSYS;
}
