/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/radeongpu: the 3D engine of an R300-class IGP (RS400/RS480), shared
 * by every process that opens it. The kernel owns the command processor:
 * programs allocate buffers in the IGP's memory, map them, and submit
 * command streams, which the kernel checks (3D state and draw packets
 * only, every address inside a buffer the program may use) and runs.
 * Buffers have global handles so another program can open them (a window
 * server sampling a client's frame); one buffer at a time can be what the
 * panel shows. */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

#define RADEON_IOC_INFO     0x52001     /* struct radeon_info */
#define RADEON_IOC_BO_NEW   0x52002     /* struct radeon_bo: size in; handle, offset out */
#define RADEON_IOC_BO_FREE  0x52003     /* uint32_t handle */
#define RADEON_IOC_BO_OPEN  0x52004     /* struct radeon_bo: handle in; size, offset out */
#define RADEON_IOC_SUBMIT   0x52005     /* struct radeon_submit */
#define RADEON_IOC_WAIT     0x52006     /* struct radeon_wait */
#define RADEON_IOC_SCANOUT  0x52007     /* struct radeon_scanout; handle 0: the console again */

struct radeon_info {
    uint32_t vram_size;         /* bytes */
    uint32_t vram_mc;           /* the IGP's address of VRAM offset 0 */
    uint32_t width, height;     /* the panel's mode */
    uint32_t chip;              /* PCI device id */
    uint32_t fence;             /* the last fence done */
};

struct radeon_bo {
    uint32_t handle;
    uint32_t size;              /* bytes (rounded up to pages) */
    uint32_t offset;            /* in VRAM: mmap this offset; the GPU sees vram_mc + offset */
    uint32_t flags;
};

struct radeon_submit {
    uint64_t cmds;              /* dwords: PACKET0 register writes and PACKET3 draws */
    uint32_t dwords;
    uint32_t fence;             /* out: signalled when the GPU has run all of it */
};

struct radeon_wait {
    uint32_t fence;
    uint32_t timeout_ms;
};

struct radeon_scanout {
    uint32_t handle;            /* a buffer of this process's, or 0 */
    uint32_t pitch;             /* bytes per row; 32 bpp, the panel's size */
};
