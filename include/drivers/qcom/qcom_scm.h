/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Calls into a Qualcomm SoC's TrustZone (SCM, over SMCCC). */
#ifndef ASM_QCOM_SCM_H
#define ASM_QCOM_SCM_H
#include "types.h"

struct scm_res { uint64_t a0, a1, a2, a3; };

#define SCM_PIL         2       /* peripheral image loader: PAS calls */
#define SCM_ARG_RW      2       /* arginfo: a buffer argument */
#define SCM_ARG_RO      1
#define SCM_ARGS(n, t1, t2, t3) ((n) | (t1) << 4 | (t2) << 6 | (t3) << 8)
#define SCM_ARGS7(t1, t2, t3, t4, t5, t6, t7) (7 | (t1) << 4 | (t2) << 6 | (t3) << 8 | (t4) << 10 | (t5) << 12 | (t6) << 14 | (t7) << 16)
#define SCM_SVC_MP      0x0c
#define SCM_MP_ASSIGN   0x16
#define SCM_VMID_HLOS   3
#define SCM_VMID_MSS_MSA 15
#define SCM_VMID_NAV    0x2b
#define SCM_VMID_WLAN   0x18
#define SCM_VMID_WLAN_CE 0x19
#define SCM_PERM_RW     6

/* svc/cmd, the argument count and kinds, three arguments; a0 the status. */
struct scm_res qcom_scm_call(uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c);
struct scm_res qcom_scm_call_n(uint32_t svc, uint32_t cmd, uint32_t arginfo, const uint64_t *args, int n);
int  qcom_scm_assign_mem(uint64_t phys, uint64_t size, const uint32_t *vmids, const uint32_t *perms, int n);
void qcom_dma_clean(const void *p, size_t len);     /* clean+invalidate to the point of coherency */

#endif
