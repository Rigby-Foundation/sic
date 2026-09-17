/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* powerpc boot-time initialisation, called from the generic kernel_main. */
#include "asm/arch.h"
#include "asm/cpu.h"
#include "asm/timer.h"
#include "asm/of.h"
#include "string.h"

struct cpu cpus[MAX_CPUS];
uint32_t cpu_count;

extern void ppc_openpic_init(void);

void cpu_set_kernel_stack(uint64_t sp)
{
    this_cpu()->kernel_sp = (uint32_t)sp;
}

void arch_cpu_init_boot(void)
{
    /* SPRG3 and the vectors were set up before kernel_main. */
}

void arch_init_interrupts(const struct zaeboot_info *info)
{
    (void)info;
    ppc_openpic_init();
    timer_init();
}

void arch_init_smp(void) { }

void arch_halt_forever(void)
{
    for (;;)
        interrupts_disable();
}

void arch_hypervisor_id(char out[13])
{
    /* No CPUID here: the firmware's name has to do. Apple's is
     * "OpenFirmware 3", QEMU's is OpenBIOS. */
    memset(out, 0, 13);
    if (platform.is_qemu)
        memcpy(out, "QEMUOpenBIOS", 12);
}
