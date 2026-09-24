/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
void virtio_gpu_init(void);         /* takes the display over if a virtio-gpu is present */
void virtio_input_init(void);       /* virtio keyboards and mice -> the console and /dev/mouse */
int  virtio_gpu_present_now(void);  /* push the framebuffer to the host now; -1 if no such device */
