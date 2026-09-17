/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* kernel/lib/symtab.c: name kernel addresses in diagnostics */
const char *ksym_name(uint64_t addr, uint64_t *off);
void        kprint_sym(uint64_t addr);
