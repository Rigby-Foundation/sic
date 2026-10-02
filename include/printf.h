/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Into a buffer: at most cap-1 characters and a NUL; returns the length the whole text needs. */
int  ksnprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  kvsnprintf(char *buf, size_t cap, const char *fmt, __builtin_va_list ap);

/* kernel/lib/symtab.c: name kernel addresses in diagnostics */
const char *ksym_name(uint64_t addr, uint64_t *off);
void        kprint_sym(uint64_t addr);
size_t klog_read(uint64_t off, char *dst, size_t n);   /* the kernel log (/proc/kmsg), from its oldest kept byte */
