/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define TIMER_HZ 1000

void     timer_init(void);      /* LAPIC timer if available, else PIT via the PIC */
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
const char *timer_source(void);   /* "lapic", "pit", "decrementer": for diagnostics */
