/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "proc/wait.h"

/* The entry is registered before the condition is checked and task_block()
 * tolerates a wake that raced ahead (wake_pending), so no wakeup is lost. */
void __wait_add(struct waitqueue *wq, struct wait_entry *e)
{
    uint64_t f = spin_lock_irqsave(&wq->lock);
    e->next = wq->head;
    wq->head = e;
    spin_unlock_irqrestore(&wq->lock, f);
}

void __wait_remove(struct waitqueue *wq, struct wait_entry *e)
{
    uint64_t f = spin_lock_irqsave(&wq->lock);
    for (struct wait_entry **pp = &wq->head; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    spin_unlock_irqrestore(&wq->lock, f);
}

void waitqueue_wake_all(struct waitqueue *wq)
{
    uint64_t f = spin_lock_irqsave(&wq->lock);
    for (struct wait_entry *e = wq->head; e; e = e->next)
        task_wake(e->task);
    spin_unlock_irqrestore(&wq->lock, f);
}
