/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"
void keyboard_init(void);
void keyboard_scancode(uint8_t raw);   /* one scancode-set-1 byte from any keyboard */
struct waitqueue;
struct file;
long keyboard_read(struct file *f, char *buf, size_t len, int nonblock);
int  keyboard_poll(struct file *f, struct waitqueue **wq);      /* POLLIN if input is waiting for this reader */
int  keyboard_set_mode(struct file *owner, int mode);   /* K_RAW / K_XLATE */
int  keyboard_get_mode(void);
void keyboard_release(struct file *f);
void     keyboard_set_foreground(uint32_t pid);
uint32_t keyboard_get_foreground(void);
