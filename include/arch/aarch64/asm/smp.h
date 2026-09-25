/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#ifdef CONFIG_SMP
uint32_t smp_cpu_count(void);       /* CPUs online */
void smp_init(void);
#else
static inline uint32_t smp_cpu_count(void) { return 1; }
#endif
