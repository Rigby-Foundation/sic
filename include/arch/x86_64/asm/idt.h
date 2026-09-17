/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#include "asm/irq.h"

void idt_init(void);
void idt_load_current(void);        /* lidt on the calling CPU (APs) */
void irq_use_apic(void);            /* switch masking/EOI from the PIC to the APICs */
