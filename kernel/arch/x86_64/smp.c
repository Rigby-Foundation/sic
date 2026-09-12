/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Symmetric multiprocessing: bring up application processors. */
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "arch/x86_64/pit.h"
#include "proc/sched.h"
#include "string.h"
#include "printf.h"
#include "arch/x86_64/idt.h"
#include "proc/syscall.h"

#define TRAMPOLINE_PHYS 0x8000
#define AP_STACK_PAGES  8

extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint64_t ap_param_cr3, ap_param_stack, ap_param_entry, ap_param_cpu;

static volatile uint32_t online_cpus = 1;

/* Address of a trampoline symbol inside the copy at 0x8000. */
static void *tramp(void *sym)
{
    return (uint8_t *)TRAMPOLINE_PHYS + ((uint8_t *)sym - ap_trampoline_start);
}

static void ap_main(struct cpu *c)
{
    cpu_init(c, c->lapic_id);
    cpu_enable_fpu();
    idt_load_current();
    syscall_init_cpu();
    lapic_init_ap();
    lapic_timer_start();

    sched_init_ap(c);               /* creates this CPU's idle task, sets current */
    __atomic_add_fetch(&online_cpus, 1, __ATOMIC_SEQ_CST);
    c->online = 1;

    sched_enter_idle();             /* never returns */
}

static int start_ap(struct cpu *c)
{
    void *stack = heap_alloc_pages(AP_STACK_PAGES);
    if (!stack)
        return -1;

    *(uint64_t *)tramp(&ap_param_cr3)   = vmm_kernel_pml4();
    *(uint64_t *)tramp(&ap_param_stack) = (uint64_t)stack + AP_STACK_PAGES * 4096;
    *(uint64_t *)tramp(&ap_param_entry) = (uint64_t)ap_main;
    *(uint64_t *)tramp(&ap_param_cpu)   = (uint64_t)c;

    /* INIT, then two SIPIs pointing at the trampoline page (vector = addr >> 12). */
    lapic_send_init(c->lapic_id);
    pit_wait_ms(10);
    lapic_send_sipi(c->lapic_id, TRAMPOLINE_PHYS >> 12);
    pit_wait_ms(1);
    if (!c->online)
        lapic_send_sipi(c->lapic_id, TRAMPOLINE_PHYS >> 12);

    for (int i = 0; i < 200 && !c->online; i++)
        pit_wait_ms(1);
    return c->online ? 0 : -1;
}

void smp_init(void)
{
    const struct acpi_madt_info *m = acpi_madt();
    if (!m || !apic_enabled() || m->cpu_count < 2) {
        kprintf("smp: single CPU\n");
        return;
    }

    memcpy((void *)TRAMPOLINE_PHYS, ap_trampoline_start, ap_trampoline_end - ap_trampoline_start);

    uint32_t bsp = lapic_id();
    for (uint32_t i = 0; i < m->cpu_count && cpu_count < MAX_CPUS; i++) {
        if (m->lapic_ids[i] == bsp)
            continue;
        struct cpu *c = &cpus[cpu_count];
        c->index = cpu_count;
        c->lapic_id = m->lapic_ids[i];
        cpu_count++;
        if (start_ap(c) == 0)
            kprintf("smp: cpu %u (lapic %u) online\n", c->index, c->lapic_id);
        else
            kprintf("smp: cpu %u (lapic %u) failed to start\n", c->index, c->lapic_id);
    }
    kprintf("smp: %u cpu(s) online\n", online_cpus);
}

uint32_t smp_cpu_count(void) { return online_cpus; }
