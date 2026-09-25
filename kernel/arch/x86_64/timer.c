/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "asm/timer.h"
#include "asm/idt.h"
#include "asm/apic.h"
#include "asm/pit.h"
#include "proc/sched.h"
#include "core/prof.h"
#include "printf.h"
#include "asm/cpu.h"

static volatile uint64_t ticks;

static void timer_irq(struct interrupt_frame *f)
{
    (void)f;
    if (this_cpu()->index == 0)
        ticks++;
    prof_tick(f);
    sched_tick();
}

void timer_init(void)
{
    irq_install(0, timer_irq);
    if (apic_enabled()) {
        lapic_timer_init(IRQ_BASE + 0, TIMER_HZ);
    } else {
        pit_start_periodic(TIMER_HZ);
        irq_unmask(0);
    }
    kprintf("timer: %u Hz via %s\n", TIMER_HZ, apic_enabled() ? "lapic" : "pit");
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void)    { return ticks * 1000 / TIMER_HZ; }

const char *timer_source(void)
{
    return apic_enabled() ? "lapic" : "pit";
}
