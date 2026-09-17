/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

/* Legacy 8254 PIT. Only used as a polled reference clock for calibration
 * and as the fallback tick source when there's no APIC. */
void     pit_wait_ms(uint32_t ms);          /* busy-wait using channel 2 */
void     pit_start_periodic(uint32_t hz);   /* channel 0 -> IRQ0 */
