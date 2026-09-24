/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

/* Linux-compatible framebuffer ioctl requests */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

/* sic: on a display that only shows the framebuffer when asked (virtio-gpu),
 * push it to the screen now; a no-op on scanout hardware. */
#define FBIOPRESENT 0x46F0
/* ... just this rectangle of it (a struct fb_rect): what a compositor
 * changed, instead of the whole screen every time. */
#define FBIOPRESENT_RECT 0x46F1
struct fb_rect { uint32_t x, y, w, h; };

/* Console display mode ioctls */
#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

/* Keyboard mode ioctls (on /dev/console): K_RAW delivers PS/2 set 1 scancodes
 * as bytes (make and break codes, 0xE0 prefixes included), no echo, no ^C. */
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define K_RAW   0x00
#define K_XLATE 0x01

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};
