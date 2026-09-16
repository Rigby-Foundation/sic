/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* kcrash: deliberately faults inside the kernel on load, to exercise the
 * exception dump (symbolised rip, backtrace). `insmod /lib/modules/kcrash.ko`
 * on a machine you don't mind halting. */
#include "module.h"
#include "printf.h"

const char module_name[] = "kcrash";

static void __attribute__((noinline)) boom(volatile int *p)
{
    kprintf("kcrash: touching %p\n", (void *)p);
    *p = 1;
}

int init_module(void)
{
    boom((volatile int *)0xFFFFD00000000010);   /* nothing is mapped there */
    return 0;
}

void cleanup_module(void) {}
