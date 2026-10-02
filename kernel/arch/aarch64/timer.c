/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The generic timer's virtual counter (CNTV): a tick every 1/TIMER_HZ s,
 * on the PPI the device tree names (27 on most boards). */
#include "asm/timer.h"
#include "asm/irq.h"
#include "asm/fdt.h"
#include "asm/sysreg.h"
#include "asm/cpu.h"
#include "proc/sched.h"
#include "core/prof.h"
#include "printf.h"

#define TIMER_IRQ (platform.timer_irq ? platform.timer_irq : 27)

static uint32_t reload;
static volatile uint64_t ticks;

/* The virtual counter, read until two reads agree: a Cortex-A73
 * (erratum 858921) can return a wrong value as the low word rolls over. */
static uint64_t counter(void)
{
    uint64_t a = read_sysreg(cntvct_el0), b = read_sysreg(cntvct_el0);
    while (b - a > 0x100000) { a = b; b = read_sysreg(cntvct_el0); }
    return b;
}

/* The next tick as an absolute deadline, from a counter value we read
 * ourselves (TVAL leaves the arithmetic to the CPU). If the timer still
 * fires right after, keep what it looked like for gic.c to report. */
struct timer_diag timer_diag[MAX_CPUS];

static void rearm(void)
{
    uint64_t now = counter();
    write_sysreg(cntv_cval_el0, now + reload);
    isb();
    uint64_t ctl = read_sysreg(cntv_ctl_el0);
    if (ctl & 4) {                                  /* ISTATUS: still due */
        struct timer_diag *d = &timer_diag[this_cpu()->index];
        if (!d->seen) { d->seen = 1; d->cnt = now; d->cval = read_sysreg(cntv_cval_el0); d->ctl = ctl; d->cnt2 = read_sysreg(cntvct_el0); }
    }
}

static void timer_irq(struct interrupt_frame *f)
{
    (void)f;
    rearm();
    if (this_cpu()->index == 0)
        ticks++;
    prof_tick(f);
    sched_tick();                               /* the switch happens after the acknowledge, in gic.c */
}

/* Each CPU's own timer (a PPI): programmed by the CPU it belongs to. */
void timer_init_cpu(void)
{
    irq_unmask(TIMER_IRQ);
    write_sysreg(cntv_ctl_el0, 1);              /* enable, unmasked */
    rearm();
}

void timer_init(void)
{
    uint32_t freq = platform.timer_freq ? platform.timer_freq : (uint32_t)read_sysreg(cntfrq_el0);
    reload = freq / TIMER_HZ;
    if (!reload) reload = 62500;
    irq_install(TIMER_IRQ, timer_irq);
    timer_init_cpu();
    kprintf("timer: %u Hz via the generic timer (%u Hz counter, interrupt %d)\n", TIMER_HZ, freq, TIMER_IRQ);
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void)    { return ticks * 1000 / TIMER_HZ; }
const char *timer_source(void) { return "generic timer"; }
