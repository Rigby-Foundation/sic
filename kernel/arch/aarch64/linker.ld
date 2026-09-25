/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The kernel runs in the direct map at KERNEL_VIRT_BASE + physical. QEMU
 * virt puts RAM at 1 GiB and loads an Image at RAM base + text_offset
 * (2 MiB, from the header in head.S); the ELF's physical addresses say the
 * same so `-kernel sic.elf` lands in the same place. */
ENTRY(_head)

KERNEL_VIRT_BASE = 0xffff000000000000;
KERNEL_PHYS_LOAD = 0x40200000;

SECTIONS
{
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD;
    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VIRT_BASE) { KEEP(*(.text.head)) *(.text .text.*) }
    . = ALIGN(2048);
    .vectors : AT(ADDR(.vectors) - KERNEL_VIRT_BASE) { KEEP(*(.vectors)) }
    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) { *(.rodata .rodata.*) }
    .ksymtab : AT(ADDR(.ksymtab) - KERNEL_VIRT_BASE) { __ksymtab_start = .; KEEP(*(.ksymtab)) __ksymtab_end = .; }
    .data : AT(ADDR(.data) - KERNEL_VIRT_BASE) { *(.data .data.* .got .got.plt) }
    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE) { __bss_start = .; *(COMMON) *(.bss .bss.*) . = ALIGN(4096); *(.bss.pagetables) __bss_end = .; }

    _kernel_end = .;
    _image_size = _kernel_end - _kernel_start;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
