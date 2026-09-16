/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET  10
#define FUTEX_PRIVATE     128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK   (~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME))

long sys_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utimeout, uint64_t uaddr2, uint32_t val3);

/* Wake up to n waiters keyed on a physical address (used on thread exit). */
int futex_wake_phys(uint64_t phys, int n);
