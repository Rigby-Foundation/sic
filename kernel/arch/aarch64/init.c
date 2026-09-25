/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* aarch64 boot-time initialisation, called from the generic kernel_main. */
#include "asm/arch.h"
#include "asm/cpu.h"
#include "asm/timer.h"
#include "asm/fdt.h"
#include "asm/sysreg.h"
#include "asm/smp.h"
#include "string.h"

struct cpu cpus[MAX_CPUS];
uint32_t cpu_count;

extern void gic_init(void);

void cpu_set_kernel_stack(uint64_t sp)
{
    this_cpu()->kernel_sp = sp;
}

void arch_cpu_init_boot(void)
{
    /* The vectors, the per-CPU block and the MMU were set up in head.S. */
    extern void kprintf(const char *, ...);
    kprintf("device tree: %s at %llx (%u bytes)%s: ram %llx+%llx, uart %llx irq %d, gic %llx/%llx, ecam %llx, pci mem %llx+%llx io %llx+%llx, %d irq map entries\n",
            platform.dtb_ok ? platform.model : "unreadable", platform.dtb_phys, platform.dtb_size, platform.dtb_ok ? "" : " (defaults)",
            platform.mem_base, platform.mem_size, platform.uart_base, platform.uart_irq, platform.gicd_base, platform.gicc_base,
            platform.ecam_base, platform.pci_mem_base, platform.pci_mem_size, platform.pci_io_base, platform.pci_io_size, platform.pci_irq_count);
}

void arch_init_interrupts(const struct zaeboot_info *info)
{
    (void)info;
    gic_init();
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
    for (;;) {
        interrupts_disable();
        __asm__ volatile("wfi");
    }
}

void arch_hypervisor_id(char out[13])
{
    /* No CPUID; the device tree's model string is the closest thing. */
    memset(out, 0, 13);
    if (strstr(platform.model, "QEMU") || strstr(platform.model, "virt"))
        memcpy(out, "QEMU virt", 9);
}
