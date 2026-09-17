/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Entry from OpenFirmware. IEEE 1275 PowerPC binding: r5 = client interface
 * entry, r3/r4 = initrd address/size (QEMU/yaboot convention), MSR has
 * IR/DR on with the firmware's 1:1 translations, r1 = a firmware stack.
 *
 * We run at physical KERNEL_PHYS_LOAD but are linked at
 * KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD, so the first job is a BAT that maps
 * the direct window (0xC0000000 -> 0) and a jump into it. The firmware's
 * hash table (SDR1) and segment registers are left alone until the early
 * code has finished talking to it. */
#include "asm/ppc_regs.h"

    .section .text.head, "ax"
    .globl _start
_start:
    mr      31, 5                   /* OF client interface */
    mr      30, 3                   /* initrd start (physical) */
    mr      29, 4                   /* initrd size */

    /* Direct map: three 256 MiB BATs, kernel RW, memory-coherent. */
    lis     3, 0xC000
    ori     3, 3, 0x1FFE            /* BEPI 0xC0000000, BL = 256 MiB, Vs */
    li      4, 0x12                 /* BRPN 0, WIMG = 0010 (M), PP = RW */
    mtspr   SPRN_DBAT0U, 3
    mtspr   SPRN_DBAT0L, 4
    mtspr   SPRN_IBAT0U, 3
    mtspr   SPRN_IBAT0L, 4
    lis     3, 0xD000
    ori     3, 3, 0x1FFE
    lis     4, 0x1000
    ori     4, 4, 0x12
    mtspr   SPRN_DBAT1U, 3
    mtspr   SPRN_DBAT1L, 4
    mtspr   SPRN_IBAT1U, 3
    mtspr   SPRN_IBAT1L, 4
    lis     3, 0xE000
    ori     3, 3, 0x1FFE
    lis     4, 0x2000
    ori     4, 4, 0x12
    mtspr   SPRN_DBAT2U, 3
    mtspr   SPRN_DBAT2L, 4
    mtspr   SPRN_IBAT2U, 3
    mtspr   SPRN_IBAT2L, 4
    /* PCI memory space 0x80000000-0xBFFFFFFF identity mapped, cache
     * inhibited + guarded, data only (DBAT3, DBAT4-6 of the 7450). */
    lis     3, 0x8000
    ori     3, 3, 0x1FFE
    lis     4, 0x8000
    ori     4, 4, 0x2A                  /* WIMG = 0101 (I, G), PP = RW */
    mtspr   SPRN_DBAT3U, 3
    mtspr   SPRN_DBAT3L, 4
    lis     3, 0x9000
    ori     3, 3, 0x1FFE
    lis     4, 0x9000
    ori     4, 4, 0x2A
    mtspr   SPRN_DBAT4U, 3
    mtspr   SPRN_DBAT4L, 4
    lis     3, 0xA000
    ori     3, 3, 0x1FFE
    lis     4, 0xA000
    ori     4, 4, 0x2A
    mtspr   SPRN_DBAT5U, 3
    mtspr   SPRN_DBAT5L, 4
    lis     3, 0xB000
    ori     3, 3, 0x1FFE
    lis     4, 0xB000
    ori     4, 4, 0x2A
    mtspr   SPRN_DBAT6U, 3
    mtspr   SPRN_DBAT6L, 4
    /* HID0: enable the extra BATs (7450: HID0[HIGH_BAT_EN], bit 23) */
    mfspr   3, SPRN_HID0
    ori     3, 3, 0x0100
    mtspr   SPRN_HID0, 3
    isync

    /* Into the direct window at our linked address. */
    lis     3, 1f@h
    ori     3, 3, 1f@l
    mtctr   3
    bctr
1:
    /* Clear .bss (the loader only copies the file). */
    lis     3, __bss_start@h
    ori     3, 3, __bss_start@l
    lis     4, __bss_end@h
    ori     4, 4, __bss_end@l
    li      0, 0
2:  cmplw   3, 4
    bge     3f
    stw     0, 0(3)
    addi    3, 3, 4
    b       2b
3:
    /* Our own stack, in .bss. */
    lis     1, boot_stack_top@h
    ori     1, 1, boot_stack_top@l
    addi    1, 1, -16
    li      0, 0
    stw     0, 0(1)                 /* back chain terminator */

    mr      3, 31                   /* of entry */
    mr      4, 30                   /* initrd */
    mr      5, 29
    bl      ppc_early_main
4:  b       4b

    .section .bss
    .balign 16
    .globl boot_stack
boot_stack:
    .space 16384
    .globl boot_stack_top
boot_stack_top:
    .globl stack_top
    .set stack_top, boot_stack_top
