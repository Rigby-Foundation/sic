/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int    memcmp(const void *a, const void *b, size_t n);
int    strcmp(const char *a, const char *b);
void  *memmove(void *dst, const void *src, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strstr(const char *h, const char *n);
