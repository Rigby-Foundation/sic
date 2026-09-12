/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
void keyboard_init(void);
#include "types.h"
long keyboard_read(char *buf, size_t len);      /* blocking */
extern const struct dev_ops console_ops;        /* /dev/console */
