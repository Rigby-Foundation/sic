/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* An initrd inside the kernel image, for loaders that will not hand one
 * over the usual way: `make ARCH=aarch64 EMBED_INITRD=path/to/initrd.tar`.
 * boot.c falls back to it when the device tree names no usable initrd. */
    .section .rodata.initrd, "a"
    .balign 4096
    .globl embedded_initrd_start, embedded_initrd_end
embedded_initrd_start:
#ifdef EMBED_INITRD
    .incbin EMBED_INITRD
#endif
embedded_initrd_end:
    .balign 4096
