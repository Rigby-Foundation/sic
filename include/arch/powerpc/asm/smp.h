/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
static inline uint32_t smp_cpu_count(void) { return 1; }   /* no SMP on the 32-bit port yet */
