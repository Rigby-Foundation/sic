/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void pic_init(void);            /* remap to IRQ_BASE, mask everything */
void pic_disable(void);         /* mask both chips entirely (APIC mode) */
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_eoi(uint8_t irq);
int  pic_is_spurious(uint8_t irq);
