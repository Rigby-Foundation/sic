/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* x86_64 boot-time initialisation, called from the generic kernel_main. */
#include "asm/arch.h"
#include "asm/cpu.h"
#include "asm/idt.h"
#include "asm/pic.h"
#include "asm/acpi.h"
#include "asm/apic.h"
#include "asm/timer.h"
#include "asm/smp.h"
#include "proc/syscall.h"
#include "zaeboot.h"
#include "string.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1 << 0)

extern void syscall_entry(void);

void syscall_init_cpu(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* syscall: CS = 0x08, SS = 0x10; sysret: CS = 0x10+16 | 3, SS = 0x10+8 | 3 */
    wrmsr(MSR_STAR, ((uint64_t)SEL_KDATA << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 8) | (1 << 10) | (1 << 18));   /* IF, TF, DF, AC */
}

void arch_cpu_init_boot(void)
{
    cpus[0].index = 0;
    cpu_count = 1;
    cpu_init(&cpus[0], 0);
    cpu_enable_fpu();
    idt_init();
    syscall_init_cpu();
}

/* PIC remapped and masked, then APICs from the MADT if we have them, then
 * the timer (LAPIC or PIT). */
void arch_init_interrupts(const struct zaeboot_info *info)
{
    pic_init();
    if (acpi_init(info->rsdp) == 0 && apic_init() == 0)
        irq_use_apic();
    timer_init();
}

void arch_init_smp(void)
{
#ifdef CONFIG_SMP
    smp_init();
#endif
}

void arch_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void arch_wake_cpu(struct cpu *c)
{
#ifdef CONFIG_SMP
    if (c != this_cpu() && c->online)
        lapic_send_ipi(c->lapic_id, 241);       /* IPI_RESCHED */
#else
    (void)c;
#endif
}

void arch_hypervisor_id(char out[13])
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    memset(out, 0, 13);
    if (!(ecx & (1u << 31)))            /* no hypervisor present bit */
        return;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x40000000));
    memcpy(out, &ebx, 4);
    memcpy(out + 4, &ecx, 4);
    memcpy(out + 8, &edx, 4);
}
