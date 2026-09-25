/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The generic timer's virtual counter (CNTV): a tick every 1/TIMER_HZ s,
 * PPI 27 on the GIC. */
#include "asm/timer.h"
#include "asm/irq.h"
#include "asm/fdt.h"
#include "asm/sysreg.h"
#include "asm/cpu.h"
#include "proc/sched.h"
#include "core/prof.h"
#include "printf.h"

#define TIMER_IRQ 27

static uint32_t reload;
static volatile uint64_t ticks;

static void timer_irq(struct interrupt_frame *f)
{
    (void)f;
    write_sysreg(cntv_tval_el0, reload);
    if (this_cpu()->index == 0)
        ticks++;
    prof_tick(f);
    sched_tick();                               /* the switch happens after the acknowledge, in gic.c */
}

/* Each CPU's own timer (a PPI): programmed by the CPU it belongs to. */
void timer_init_cpu(void)
{
    irq_unmask(TIMER_IRQ);
    write_sysreg(cntv_tval_el0, reload);
    write_sysreg(cntv_ctl_el0, 1);              /* enable, unmasked */
}

void timer_init(void)
{
    uint32_t freq = platform.timer_freq ? platform.timer_freq : (uint32_t)read_sysreg(cntfrq_el0);
    reload = freq / TIMER_HZ;
    if (!reload) reload = 62500;
    irq_install(TIMER_IRQ, timer_irq);
    timer_init_cpu();
    kprintf("timer: %u Hz via the generic timer (%u Hz counter)\n", TIMER_HZ, freq);
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void)    { return ticks * 1000 / TIMER_HZ; }
const char *timer_source(void) { return "generic timer"; }
