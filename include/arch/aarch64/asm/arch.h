/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "asm/irqflags.h"
struct cpu;
struct zaeboot_info;

#define ARCH_NAME "aarch64"

void arch_cpu_init_boot(void);
void arch_init_interrupts(const struct zaeboot_info *info);
void arch_init_smp(void);
void arch_halt_forever(void) __attribute__((noreturn));
void arch_hypervisor_id(char out[13]);
extern int aarch64_idle_poll;           /* "idle=poll": spin instead of wfi (a Qualcomm SoC's low-power states, suspected) */
static inline void arch_idle(void)
{
    interrupts_enable();
    if (aarch64_idle_poll) { for (int i = 0; i < 256; i++) __asm__ volatile("yield" ::: "memory"); return; }
    __asm__ volatile("wfi" ::: "memory");
}
void arch_wake_cpu(struct cpu *c);      /* a reschedule SGI (gic.c) */
