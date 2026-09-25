/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* What the generic kernel asks of an architecture at boot
 * (kernel/arch/<arch>/init.c). Every arch provides this header. */
#pragma once
#include "types.h"
#include "asm/irqflags.h"
struct cpu;

struct zaeboot_info;

#define ARCH_NAME "x86_64"

/* The boot CPU: descriptor tables, FPU, syscall entry; before any printing
 * beyond the serial port. */
void arch_cpu_init_boot(void);
/* Interrupt controllers and the periodic timer; interrupts stay disabled. */
void arch_init_interrupts(const struct zaeboot_info *info);
/* Secondary CPUs (CONFIG_SMP), after the scheduler runs. */
void arch_init_smp(void);
/* Stop for good: interrupts off, halted. */
void arch_halt_forever(void) __attribute__((noreturn));
/* "TCGTCGTCGTCG", "KVMKVMKVM\0\0\0", ... or empty on bare metal (self tests
 * only format disks inside a VM). */
void arch_hypervisor_id(char out[13]);
/* Wait for the next interrupt with interrupts enabled (the boot task's idle loop). */
static inline void arch_idle(void) { __asm__ volatile("sti; hlt"); }
/* Kick a CPU out of arch_idle() so it looks at the run queue now. */
void arch_wake_cpu(struct cpu *c);
