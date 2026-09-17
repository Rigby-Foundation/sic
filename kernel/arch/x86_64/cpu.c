/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "asm/cpu.h"
#include "string.h"

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

struct cpu cpus[MAX_CPUS];
uint32_t   cpu_count;

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* GDT layout (index = selector / 8):
 *   1 kernel code   2 kernel data   3 user data   4 user code   5-6 TSS
 * sysret sets CS = STAR[63:48] + 16 and SS = STAR[63:48] + 8, so with
 * STAR[63:48] = 0x10 we land on 0x20|3 / 0x18|3, exactly this order. */
void cpu_init(struct cpu *c, uint32_t lapic_id)
{
    c->self = c;
    c->lapic_id = lapic_id;

    memset(&c->tss, 0, sizeof(c->tss));
    c->tss.iomap_base = sizeof(c->tss);
    uint64_t tb = (uint64_t)&c->tss, tl = sizeof(c->tss) - 1;

    c->gdt[0] = 0;
    c->gdt[1] = 0x00209A0000000000;     /* kernel code: L, P, DPL0 */
    c->gdt[2] = 0x0000920000000000;     /* kernel data */
    c->gdt[3] = 0x0000F20000000000;     /* user data:   P, DPL3, RW */
    c->gdt[4] = 0x0020FA0000000000;     /* user code:   L, P, DPL3 */
    c->gdt[5] = (tl & 0xFFFF) | ((tb & 0xFFFFFF) << 16) | (0x89UL << 40) |
                (((tl >> 16) & 0xF) << 48) | (((tb >> 24) & 0xFF) << 56);
    c->gdt[6] = tb >> 32;

    struct gdt_ptr ptr = { sizeof(c->gdt) - 1, (uint64_t)c->gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "ltr %3\n"
        : : "m"(ptr), "i"(SEL_KCODE), "i"(SEL_KDATA), "r"((uint16_t)SEL_TSS) : "rax", "memory");

    /* Kernel mode uses GS_BASE; user mode sees KERNEL_GS_BASE swapped in by swapgs. */
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

struct cpu *cpu_current(void) { return this_cpu(); }

void cpu_set_kernel_stack(uint64_t rsp0)
{
    struct cpu *c = this_cpu();
    c->tss.rsp0 = rsp0;
    c->kernel_rsp = rsp0;
}

uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

void cpu_enable_fpu(void)
{
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1UL << 2) | (1UL << 3));      /* EM=0 (no emulation), TS=0 */
    cr0 |= (1UL << 1);                      /* MP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);        /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("fninit");
    static int have_template;
    if (!have_template) {
        fpu_save(fpu_initial_state);
        have_template = 1;
    }
}
