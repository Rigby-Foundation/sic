/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "abi/mouse.h"
void mouse_init(void);      /* /dev/mouse; the PS/2 mouse behind it on x86 */
void mouse_push(int dx, int dy, unsigned buttons);   /* one motion/button record from any source */
void mouse_push_abs(int x, int y, unsigned buttons); /* the pointer at a screen position (a touchscreen) */
