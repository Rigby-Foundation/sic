/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define read_sysreg(r)      ({ uint64_t _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define write_sysreg(r, v)  do { uint64_t _v = (uint64_t)(v); __asm__ volatile("msr " #r ", %0" : : "r"(_v) : "memory"); } while (0)
#define isb()               __asm__ volatile("isb" ::: "memory")
#define dsb(opt)            __asm__ volatile("dsb " #opt ::: "memory")
#define dmb(opt)            __asm__ volatile("dmb " #opt ::: "memory")

/* SPSR_EL1 */
#define PSR_MODE_EL0t 0x0
#define PSR_MODE_EL1t 0x4
#define PSR_MODE_EL1h 0x5
#define PSR_F (1 << 6)
#define PSR_I (1 << 7)
#define PSR_A (1 << 8)
#define PSR_D (1 << 9)

/* ESR_EL1 exception classes */
#define ESR_EC(esr)       (((esr) >> 26) & 0x3f)
#define ESR_ISS(esr)      ((esr) & 0x1ffffff)
#define EC_UNKNOWN        0x00
#define EC_WFI            0x01
#define EC_FP_ASIMD       0x07      /* SIMD/FP access when CPACR traps it */
#define EC_SVC64          0x15
#define EC_IABT_LOW       0x20      /* instruction abort from EL0 */
#define EC_IABT_CUR       0x21
#define EC_PC_ALIGN       0x22
#define EC_DABT_LOW       0x24      /* data abort from EL0 */
#define EC_DABT_CUR       0x25
#define EC_SP_ALIGN       0x26
#define EC_BRK64          0x3c
