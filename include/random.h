/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void random_init(void);                         /* seed it (and say from what) */
void random_bytes(void *buf, size_t len);       /* kernel memory only */
void random_add(uint64_t v);                    /* mix in something unpredictable */
