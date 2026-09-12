/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* 64-bit entry point, called by zaeboot with the boot info pointer in %rdi. */

.section .bss
.align 16
stack_bottom:
        .skip 65536
stack_top:
.global stack_top

.section .text
.code64
.global _start
_start:
        cli
        mov     $stack_top, %rsp
        xor     %rbp, %rbp
        call    kernel_main
halt:   cli
        hlt
        jmp     halt
