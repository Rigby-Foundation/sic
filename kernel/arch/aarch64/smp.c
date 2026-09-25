/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Symmetric multiprocessing: the other CPUs of the device tree, started
 * through PSCI CPU_ON (SMC or HVC, whichever the tree names) at
 * secondary_entry (head.S). */
#include "asm/smp.h"
#include "asm/cpu.h"
#include "asm/fdt.h"
#include "asm/timer.h"
#include "asm/memlayout.h"
#include "asm/sysreg.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "string.h"
#include "printf.h"

#define PSCI_CPU_ON_64  0xC4000003u
#define AP_STACK_PAGES  8

extern void secondary_entry(void);
extern void gic_init_cpu(void);
uint64_t secondary_stack;           /* what the next CPU to come up finds (head.S) */
struct cpu *secondary_cpu;

static volatile uint32_t online_cpus = 1;
static int use_hvc;

uint32_t smp_cpu_count(void) { return online_cpus; }

static long psci_call(uint32_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    if (use_hvc) __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else         __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (long)x0;
}

void aarch64_secondary_main(struct cpu *c)
{
    gic_init_cpu();
    timer_init_cpu();
    sched_init_ap(c);               /* creates this CPU's idle task, sets current */
    __atomic_add_fetch(&online_cpus, 1, __ATOMIC_SEQ_CST);
    c->online = 1;
    sched_enter_idle();             /* never returns */
}

/* The secondary starts with its caches off: what it reads before the MMU
 * is on must be in memory, not in our cache. */
static void clean_to_memory(const void *p, size_t len)
{
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + len;
    for (; a < e; a += 64) __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    dsb(sy);
}

static int start_cpu(struct cpu *c)
{
    void *stack = heap_alloc_pages(AP_STACK_PAGES);
    if (!stack) return -1;
    secondary_stack = (uint64_t)(uintptr_t)stack + AP_STACK_PAGES * 4096;
    secondary_cpu = c;
    clean_to_memory(&secondary_stack, sizeof secondary_stack);
    clean_to_memory(&secondary_cpu, sizeof secondary_cpu);
    long rc = psci_call(PSCI_CPU_ON_64, c->mpidr, V2P(secondary_entry), 0);
    if (rc != 0) { kprintf("smp: cpu %u: PSCI CPU_ON failed (%ld)\n", c->index, rc); return -1; }
    for (int i = 0; i < 500 && !c->online; i++) task_sleep_ms(1);
    return c->online ? 0 : -1;
}

void smp_init(void)
{
    int psci = fdt_path("/psci");
    if (psci < 0) { kprintf("smp: no PSCI in the device tree: single CPU\n"); return; }
    const char *method = fdt_prop(psci, "method", NULL);
    use_hvc = method && strcmp(method, "hvc") == 0;

    int cpus_node = fdt_path("/cpus"), n = 1;
    for (int c = cpus_node >= 0 ? fdt_first_child(cpus_node) : -1; c >= 0 && n < MAX_CPUS; c = fdt_next_sibling(c)) {
        const char *type = fdt_prop(c, "device_type", NULL);
        if (!type || strcmp(type, "cpu") != 0) continue;
        int len;
        const uint8_t *reg = fdt_prop(c, "reg", &len);
        if (!reg) continue;
        uint64_t mpidr = len >= 8 ? fdt_prop_u64(c, "reg", 0, 0) : fdt_prop_u32(c, "reg", 0, 0);
        if (mpidr == (read_sysreg(mpidr_el1) & 0xff00ffffffULL)) continue;      /* us */
        cpus[n].self = &cpus[n];
        cpus[n].index = (uint32_t)n;
        cpus[n].mpidr = mpidr;
        n++;
    }
    cpu_count = (uint32_t)n;
    for (int i = 1; i < n; i++)
        if (start_cpu(&cpus[i]) != 0) kprintf("smp: cpu %d did not come up\n", i);
    kprintf("smp: %u of %u cpus online (PSCI %s)\n", online_cpus, cpu_count, use_hvc ? "hvc" : "smc");
}
