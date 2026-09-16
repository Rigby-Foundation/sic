/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
void keyboard_init(void);
#include "types.h"
long keyboard_read(char *buf, size_t len, int nonblock);
struct waitqueue;
int  keyboard_poll(struct waitqueue **wq);      /* POLLIN if input is waiting */      /* blocking */
struct file;
int  keyboard_set_mode(struct file *owner, int mode);   /* K_RAW / K_XLATE */
int  keyboard_get_mode(void);
void keyboard_release(struct file *f);
void     keyboard_set_foreground(uint32_t pid);
uint32_t keyboard_get_foreground(void);
