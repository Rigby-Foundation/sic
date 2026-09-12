/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "arch/x86_64/timer.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/pit.h"
#include "proc/sched.h"
#include "printf.h"
#include "arch/x86_64/cpu.h"

static volatile uint64_t ticks;

static void timer_irq(struct interrupt_frame *f)
{
    (void)f;
    if (this_cpu()->index == 0)
        ticks++;
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
