/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "zaeboot.h"

void fb_init(const struct zaeboot_framebuffer *fb, void *virt, int in_ram);   /* base is physical, virt where to write */
extern uint32_t fb_safe_top;   /* rows hidden at the top (a camera cutout); set before fb_init */
void fb_set_present_hook(int (*fn)(void));  /* for displays that need a push (virtio-gpu): FBIOPRESENT calls it */
void fb_set_present_rect_hook(int (*fn)(uint32_t x, uint32_t y, uint32_t w, uint32_t h));   /* FBIOPRESENT_RECT */
/* The console drew here: a display that needs pushing notes it (any context, never sleeps or wakes). */
void radeon_init(void);    /* drivers/radeon.c */
int  radeon_cp_start(void);     /* drivers/radeon_cp.c: the 3D engine, /dev/radeongpu */
void fb_set_damage_hook(void (*fn)(uint32_t x, uint32_t y, uint32_t w, uint32_t h));
/* The display changed size (the same buffer, a new geometry): /dev/fb0 reports
 * it, its pollers get POLLPRI. map_len is how much of the buffer may be mapped. */
void fb_mode_change(uint32_t width, uint32_t height, uint32_t pitch, uint64_t map_len);
void fb_init_text(void);        /* 80x25 VGA text at 0xB8000 instead (BIOS boot, no VBE mode) */
void fb_set_color(uint32_t fg, uint32_t bg);   /* 0xRRGGBB */
void fb_clear(void);
void fb_force_text(void);       /* out of graphics mode, for a hang report */
void fb_putc(char c);
void fb_puts(const char *s);

void fb_dev_init(void);
int  fb_is_graphics_mode(void);
int  fb_have_framebuffer(void);
void fb_use_shadow(void);
void fb_use_shadow_copy(void);    /* the same, keeping what the screen shows */         /* keep a RAM copy for scrolling (frame buffers that are slow to read) */   /* a pixel frame buffer (not VGA text, not none) */
