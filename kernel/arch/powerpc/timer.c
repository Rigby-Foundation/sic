/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The decrementer: reloaded every tick from the timebase frequency the
 * device tree gave us. */
#include "asm/timer.h"
#include "asm/ppc_regs.h"
#include "asm/of.h"
#include "asm/ptrace.h"
#include "proc/sched.h"
#include "printf.h"

static uint32_t dec_reload;
static volatile uint64_t ticks;

static inline void mtdec(uint32_t v) { __asm__ volatile("mtdec %0" : : "r"(v)); }

void timer_init(void)
{
    dec_reload = platform.timebase_freq / TIMER_HZ;
    if (dec_reload == 0) dec_reload = 16666;
    mtdec(dec_reload);
    kprintf("timer: %u Hz via decrementer (timebase %u Hz)\n", TIMER_HZ, platform.timebase_freq);
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void)    { return ticks * 1000 / TIMER_HZ; }
const char *timer_source(void) { return "decrementer"; }

void ppc_decrementer(struct interrupt_frame *f)
{
    (void)f;
    mtdec(dec_reload);
    ticks++;
    sched_tick();
    sched_preempt();
}
