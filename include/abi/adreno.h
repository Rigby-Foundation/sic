/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/adrenogpu: a Qualcomm Adreno 6xx (the A610 of a "creek" phone),
 * shared by every process that opens it. The kernel owns the command
 * processor and the GPU's page table: programs allocate buffers, map them,
 * and submit PM4 command streams, which the CP runs from a kernel copy as an
 * indirect buffer with register protection on (CP_PROTECT: no privileged
 * registers). The GPU reaches only GPU buffers and the screen; a buffer's
 * GPU address is fixed for its life. Buffers have global handles so another
 * program can open them (a window server sampling a client's frame). */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

#define ADRENO_IOC_INFO     0x41001     /* struct adreno_info */
#define ADRENO_IOC_BO_NEW   0x41002     /* struct adreno_bo_info: size in; handle, gpuaddr, offset out */
#define ADRENO_IOC_BO_FREE  0x41003     /* uint32_t handle */
#define ADRENO_IOC_BO_OPEN  0x41004     /* struct adreno_bo_info: handle in; the rest out */
#define ADRENO_IOC_SUBMIT   0x41005     /* struct adreno_submit */
#define ADRENO_IOC_WAIT     0x41006     /* struct adreno_wait */

struct adreno_info {
    uint32_t chip_id;           /* 0x06010001: A610 */
    uint32_t gmem_size;         /* bytes of on-chip tile memory */
    uint32_t width, height;     /* the screen */
    uint64_t fb_gpuaddr;        /* the screen as the GPU sees it: B8G8R8A8 */
    uint32_t fb_pitch;          /* bytes per row (not always 64-byte aligned) */
    uint32_t fence;             /* the last fence done */
};

struct adreno_bo_info {
    uint32_t handle;
    uint32_t size;              /* bytes (rounded up to pages) */
    uint64_t gpuaddr;           /* where the GPU sees it */
    uint64_t offset;            /* mmap this offset of /dev/adrenogpu */
};

struct adreno_submit {
    uint64_t cmds;              /* PM4 dwords (type 4 register writes, type 7 packets) */
    uint32_t dwords;
    uint32_t fence;             /* out: signalled when the GPU has run all of it and flushed */
};

struct adreno_wait {
    uint32_t fence;
    uint32_t timeout_ms;
};
