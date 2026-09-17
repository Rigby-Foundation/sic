/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The kernel runs at KERNEL_VIRT_BASE + physical; OpenFirmware (QEMU mac99,
 * PowerMacs) loads the image at physical 16 MiB. QEMU's -kernel loader puts
 * ELF segments at (paddr & 0x0fffffff) + 0x01000000 and OpenBIOS enters at
 * 0x01000000 regardless of e_entry, so the physical addresses in the ELF are
 * offsets from 0 and _start is the first byte. */
ENTRY(_start)

KERNEL_VIRT_BASE = 0xC0000000;
KERNEL_PHYS_LOAD = 0x01000000;

SECTIONS
{
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD;
    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VIRT_BASE - KERNEL_PHYS_LOAD) { KEEP(*(.text.head)) *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE - KERNEL_PHYS_LOAD) { *(.rodata .rodata.*) }
    .ksymtab : AT(ADDR(.ksymtab) - KERNEL_VIRT_BASE - KERNEL_PHYS_LOAD) { __ksymtab_start = .; KEEP(*(.ksymtab)) __ksymtab_end = .; }
    .data : AT(ADDR(.data) - KERNEL_VIRT_BASE - KERNEL_PHYS_LOAD) { *(.data .data.* .sdata .sdata.* .got .got2) }
    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE - KERNEL_PHYS_LOAD) { __bss_start = .; *(COMMON) *(.bss .bss.* .sbss .sbss.*) __bss_end = .; }

    _kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
