/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "asm/ptrace.h"
void prof_tick(const struct interrupt_frame *f);   /* from the timer interrupt, on every CPU */
void prof_init(void);                              /* /proc/prof */
