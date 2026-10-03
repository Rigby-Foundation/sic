/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Calls into a Qualcomm SoC's TrustZone (SCM, over SMCCC). */
#ifndef ASM_QCOM_SCM_H
#define ASM_QCOM_SCM_H
#include "types.h"

struct scm_res { uint64_t a0, a1, a2, a3; };

#define SCM_PIL         2       /* peripheral image loader: PAS calls */
#define SCM_ARG_RW      2       /* arginfo: a buffer argument */
#define SCM_ARGS(n, t1, t2, t3) ((n) | (t1) << 4 | (t2) << 6 | (t3) << 8)

/* svc/cmd, the argument count and kinds, three arguments; a0 the status. */
struct scm_res qcom_scm_call(uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c);

#endif
