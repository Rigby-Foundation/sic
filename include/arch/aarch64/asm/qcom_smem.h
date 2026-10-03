/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A Qualcomm SoC's shared memory with its remote processors. */
#ifndef ASM_QCOM_SMEM_H
#define ASM_QCOM_SMEM_H
#include "types.h"

#define SMEM_GLOBAL_HOST 0xfffe

/* Item `item` of the partition shared with `host` (or the global one): a
 * pointer into device memory (read it with mmio_read32), or NULL. */
void *smem_get(unsigned host, unsigned item, size_t *size);
uint64_t smem_virt_to_phys(const void *p);

#endif
