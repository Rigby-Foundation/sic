/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Wait queues: sleep until a condition holds, woken by whoever changes it. */
#pragma once
#include "types.h"
#include "spinlock.h"
#include "proc/sched.h"
#include "asm/timer.h"

struct wait_entry {
    struct task *task;
    struct wait_entry *next;
};

struct waitqueue {
    spinlock_t lock;
    struct wait_entry *head;
};

#define WAITQUEUE_INIT { SPINLOCK_INIT, NULL }

void waitqueue_wake_all(struct waitqueue *wq);

/* Internal: register/unregister the current task. */
void __wait_add(struct waitqueue *wq, struct wait_entry *e);
void __wait_remove(struct waitqueue *wq, struct wait_entry *e);

/* Block until `cond` is true. Re-evaluates cond after every wakeup, so a
 * spurious wakeup is harmless. Returns 0, or -1 if a signal became pending
 * first (interruptible sleep). */
#define wait_event_interruptible(wq, cond) ({                          \
    int __rc = 0;                                                       \
    struct wait_entry __e = { task_current(), NULL };                   \
    for (;;) {                                                          \
        __wait_add((wq), &__e);                                         \
        if (cond) { __wait_remove((wq), &__e); break; }                 \
        if (task_signal_pending(task_current())) {                      \
            __wait_remove((wq), &__e); __rc = -1; break;                \
        }                                                               \
        task_block();                                                   \
        __wait_remove((wq), &__e);                                      \
    }                                                                   \
    __rc; })

/* Like wait_event_interruptible, but gives up after `ms` milliseconds.
 * Returns 1 if cond became true, 0 on timeout, -1 on a pending signal. */
#define wait_event_timeout(wq, cond, ms) ({                              \
    int __rc = 0;                                                       \
    uint64_t __deadline = timer_ticks() + ((uint64_t)(ms) * TIMER_HZ + 999) / 1000; \
    struct wait_entry __e = { task_current(), NULL };                   \
    for (;;) {                                                          \
        __wait_add((wq), &__e);                                         \
        if (cond) { __wait_remove((wq), &__e); __rc = 1; break; }       \
        if (task_signal_pending(task_current())) {                      \
            __wait_remove((wq), &__e); __rc = -1; break;                \
        }                                                               \
        uint64_t __now = timer_ticks();                                 \
        if (__now >= __deadline) { __wait_remove((wq), &__e); break; }  \
        task_sleep_ms(((__deadline - __now) * 1000) / TIMER_HZ + 1);    \
        __wait_remove((wq), &__e);                                      \
    }                                                                   \
    __rc; })
