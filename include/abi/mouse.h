/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/mouse: a stream of these, one per motion/button report. Reads return
 * whole records (as many as fit), block until one arrives unless O_NONBLOCK,
 * and the device polls readable when one is waiting. */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

struct mouse_event {
    int16_t  dx, dy;            /* relative motion; y grows downwards */
    uint16_t buttons;           /* MOUSE_BTN_* held after this report */
    uint16_t pad;
};
#define MOUSE_BTN_LEFT   1
#define MOUSE_BTN_RIGHT  2
#define MOUSE_BTN_MIDDLE 4
