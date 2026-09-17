/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;    /* always "long long": %llx on every arch */
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned long      size_t;      /* the native word: 64 bits on x86_64, 32 on powerpc */
typedef signed long        ssize_t;
typedef unsigned long      uintptr_t;
typedef signed long        intptr_t;

typedef uint64_t           paddr_t;     /* physical addresses: 64-bit everywhere (G4s have 36-bit) */
typedef uintptr_t          vaddr_t;     /* virtual addresses: the native word */

#if __SIZEOF_POINTER__ == 8
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif

#define NULL ((void *)0)
