/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
#include "zaeboot.h"

void fb_init(const struct zaeboot_framebuffer *fb);
void fb_init_text(void);        /* 80x25 VGA text at 0xB8000 instead (BIOS boot, no VBE mode) */
void fb_set_color(uint32_t fg, uint32_t bg);   /* 0xRRGGBB */
void fb_clear(void);
void fb_putc(char c);
void fb_puts(const char *s);

void fb_dev_init(void);
int  fb_is_graphics_mode(void);
