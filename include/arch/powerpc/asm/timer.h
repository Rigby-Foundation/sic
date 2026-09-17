/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#define TIMER_HZ 1000
void     timer_init(void);      /* the decrementer, from the timebase frequency in the device tree */
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
const char *timer_source(void);
