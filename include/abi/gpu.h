/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/gpu0: the 3D side of virtio-gpu (virgl). A program opens the device
 * (which gives it a rendering context on the host), creates resources
 * (buffers and textures, backed by guest memory it can mmap), submits
 * virgl command streams, and moves data between its memory and the host
 * copy of a resource with transfers. Every operation completes before the
 * ioctl returns. The command stream format is virglrenderer's
 * (virgl_protocol.h); libvirgl encodes it. */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

#define GPU_IOC_INFO        0x47000     /* struct gpu_info */
#define GPU_IOC_CREATE_RES  0x47001     /* struct gpu_res_create, id filled in */
#define GPU_IOC_DESTROY_RES 0x47002     /* uint32_t id */
#define GPU_IOC_TRANSFER_TO_HOST   0x47003   /* struct gpu_transfer */
#define GPU_IOC_TRANSFER_FROM_HOST 0x47004   /* struct gpu_transfer */
#define GPU_IOC_SUBMIT      0x47005     /* struct gpu_submit */
#define GPU_IOC_GET_CAPSET  0x47006     /* struct gpu_capset */
/* Compositing: another program's resource used in this context (a window
 * server sampling a client's colour buffer). The resource lives until its
 * owner and every attacher are done with it. */
#define GPU_IOC_ATTACH_RES  0x47007     /* uint32_t id */
#define GPU_IOC_DETACH_RES  0x47008     /* uint32_t id */
/* The display shows a resource of this file's (a render target) instead
 * of the framebuffer; FLUSH puts a rectangle of it on screen. When the file
 * closes, the framebuffer comes back. */
#define GPU_IOC_SET_SCANOUT 0x47009     /* struct gpu_scanout */
#define GPU_IOC_FLUSH       0x4700A     /* struct gpu_scanout (the rectangle) */

struct gpu_scanout {
    uint32_t res;
    uint32_t x, y, w, h;
};

struct gpu_info {
    uint32_t ctx_id;            /* the host context this file renders in */
    uint32_t capset_id;         /* VIRTIO_GPU_CAPSET_VIRGL(2) or VIRGL2 */
    uint32_t capset_version;
    uint32_t capset_size;
};

struct gpu_res_create {
    uint32_t target;            /* PIPE_BUFFER, PIPE_TEXTURE_2D, ... */
    uint32_t format;            /* VIRGL_FORMAT_* */
    uint32_t bind;              /* VIRGL_BIND_* */
    uint32_t width, height, depth, array_size, last_level, nr_samples, flags;
    uint32_t size;              /* bytes of guest backing to allocate (0: none) */
    uint32_t id;                /* out: resource id; mmap offset = id * 4096 */
};

struct gpu_transfer {
    uint32_t res;
    uint32_t level;
    uint32_t x, y, z, w, h, d;  /* the box in the resource */
    uint32_t stride, layer_stride;
    uint64_t offset;            /* into the guest backing */
};

struct gpu_submit {
    uint64_t buf;               /* the command stream (dwords) */
    uint32_t size;              /* bytes, at most GPU_SUBMIT_MAX */
    uint32_t flags;             /* GPU_SUBMIT_* */
};
/* Wait until the host has run the stream, with a fence in this context:
 * the host flushes the context for it, which is what makes its rendering
 * visible to other contexts (the display, a compositor sampling it). */
#define GPU_SUBMIT_FENCE 1
#define GPU_SUBMIT_MAX (64 * 1024)

struct gpu_capset {
    uint64_t buf;               /* where to put it */
    uint32_t size;              /* its size; from gpu_info */
    uint32_t pad;
};
