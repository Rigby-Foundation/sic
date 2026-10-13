/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "fs/vfs.h"

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   02000000
#define EFD_NONBLOCK  04000

/* Create an eventfd; fills *out with a file object. Returns 0 or -errno. */
int eventfd_create(unsigned int initval, int flags, struct file **out);
