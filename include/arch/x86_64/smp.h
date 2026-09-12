/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void     smp_init(void);            /* start every AP listed in the MADT */
uint32_t smp_cpu_count(void);       /* CPUs online */
void     smp_ap_ready(void);        /* called by each AP once it can schedule */
