/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    _kernel_start = .;

    .multiboot : { KEEP(*(.multiboot)) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }
    .ksymtab   : { __ksymtab_start = .; KEEP(*(.ksymtab)) __ksymtab_end = .; }
    .data      : { *(.data .data.*) }
    .bss       : { *(COMMON) *(.bss .bss.*) }

    _kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
