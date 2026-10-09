/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#define TIMER_HZ 1000
void     timer_init(void);      /* the generic timer (CNTV), frequency from CNTFRQ_EL0 */
void     timer_init_cpu(void);  /* this CPU's copy of it */
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
uint64_t timer_boot_epoch(void);  /* the date at boot (a PL031), seconds since 1970; 0 if unknown */
const char *timer_source(void);

/* What the timer looked like when it stayed due after being re-armed (gic.c reports it). */
struct timer_diag { int seen; uint64_t cnt, cval, ctl, cnt2; };
