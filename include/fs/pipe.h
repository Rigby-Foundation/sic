/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "fs/vfs.h"

/* Create a pipe; fills two file objects (read end, write end). 0 or -errno. */
int pipe_create(struct file **rd, struct file **wr);
