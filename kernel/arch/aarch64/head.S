/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Entry: a Linux arm64 Image header, then the MMU-off start of the world.
 * The loader (QEMU) enters at _head with x0 = physical address of the
 * device tree, MMU and caches off, at EL1 (or EL2, which we drop out of).
 * We are linked in the direct map but run at our physical address until
 * the page tables below are live, so everything here is PC-relative. */
#define TEXT_OFFSET 0x200000

    .section .text.head, "ax"
    .globl _head
_head:
    b       _start                      /* code0: jump to the real entry */
    .long   0                           /* code1 */
    .quad   TEXT_OFFSET                 /* text_offset: where in RAM to put us */
    .quad   _image_size                 /* image_size */
    .quad   0x0a                        /* flags: little-endian, 4 KiB pages, place anywhere */
    .quad   0, 0, 0                     /* reserved */
    .ascii  "ARM\x64"                   /* magic */
    .long   0                           /* PE header offset (none) */

_start:
    mov     x20, x0                     /* the device tree, physical */

    /* EL2 (virtualization=on): set EL1 to AArch64 and drop down. */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    1f
    mov     x1, #(1 << 31)              /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x1
    mov     x1, #0x3c5                  /* SPSR: EL1h, DAIF masked */
    msr     spsr_el2, x1
    adr     x1, 1f
    msr     elr_el2, x1
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3                  /* EL1 may use the physical timer/counter */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    eret
1:
    msr     daifset, #0xf
    msr     spsel, #1                   /* SP_EL1 for the kernel */

    /* .bss (the page tables are in it) */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
2:  cmp     x1, x2
    b.hs    3f
    str     xzr, [x1], #8
    b       2b
3:
    /* Page tables. TTBR1: L0[0] -> L1 with 1 GiB blocks for the first 4 GiB
     * of physical space: [0] device (GIC, UART, PCI windows), [1..3] normal
     * memory (RAM starts at 1 GiB on QEMU virt). TTBR0: the same L1 for a
     * temporary identity map so the PC survives the switch. */
    adrp    x1, boot_l0
    adrp    x2, boot_l1
    orr     x3, x2, #3                  /* table descriptor */
    str     x3, [x1]
    adrp    x1, boot_l0_ident
    str     x3, [x1]
    /* block: AF (bit 10), SH inner (bits 8-9 = 11), AP RW EL1 (bits 6-7 = 00), AttrIdx, valid */
    mov     x3, #0x401                  /* valid block, AF */
    orr     x3, x3, #(1 << 2)           /* AttrIdx 1: device */
    str     x3, [x2]
    mov     x3, #0x701                  /* valid block, AF, SH inner */
    mov     x6, #0x40000000
    mov     x4, x6
    orr     x5, x3, x4
    str     x5, [x2, #8]
    add     x4, x4, x6
    orr     x5, x3, x4
    str     x5, [x2, #16]
    add     x4, x4, x6
    orr     x5, x3, x4
    str     x5, [x2, #24]

    bl      mmu_on
    /* Into the direct map at our linked address; the identity map goes. */
    ldr     x1, =4f
    br      x1
4:  msr     ttbr0_el1, xzr
    tlbi    vmalle1
    dsb     nsh
    isb
    ldr     x1, =stack_top
    mov     sp, x1
    ldr     x1, =vectors
    msr     vbar_el1, x1
    ldr     x1, =cpus
    msr     tpidr_el1, x1
    mov     x0, x20
    bl      aarch64_early_main
5:  wfi
    b       5b

/* Translation on, with the tables above (already built by the boot CPU
 * for a secondary). PC-relative only: we still run at the physical address. */
mmu_on:
    /* MAIR: 0 normal write-back, 1 device nGnRnE, 2 device nGnRE, 3 normal non-cacheable */
    ldr     x1, =0x440400ff
    msr     mair_el1, x1
    /* TCR: 48-bit both halves, 4 KiB granules, inner-shareable write-back walks, PA range from the CPU */
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #0xf                /* PARange */
    cmp     x1, #5
    csel    x1, x1, xzr, ls
    lsl     x1, x1, #32                 /* IPS */
    movz    x2, #0x3510                 /* T0SZ=16, IRGN0/ORGN0=1, SH0=3 (bits 0-13: 0x3510) */
    movk    x2, #0xb510, lsl #16        /* T1SZ=16, IRGN1/ORGN1=1, SH1=3, TG1=4K (10) */
    orr     x1, x1, x2
    msr     tcr_el1, x1
    adrp    x1, boot_l0
    msr     ttbr1_el1, x1
    adrp    x1, boot_l0_ident
    msr     ttbr0_el1, x1
    /* FP/SIMD for EL0 (the kernel itself is -mgeneral-regs-only) */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)           /* M */
    orr     x1, x1, #(1 << 2)           /* C */
    orr     x1, x1, #(1 << 12)          /* I */
    bic     x1, x1, #(1 << 1)           /* A: no alignment checking */
    bic     x1, x1, #(1 << 3)           /* SA */
    bic     x1, x1, #(1 << 4)           /* SA0 */
    bic     x1, x1, #(1 << 19)          /* WXN */
    orr     x1, x1, #(1 << 14)          /* DZE: dc zva from EL0 */
    orr     x1, x1, #(1 << 15)          /* UCT: CTR_EL0 readable from EL0 */
    orr     x1, x1, #(1 << 26)          /* UCI: cache maintenance from EL0 (__clear_cache) */
    msr     sctlr_el1, x1
    mov     x1, #3
    msr     cntkctl_el1, x1             /* the counters readable from EL0 */
    isb
    ret

/* A secondary CPU, from PSCI CPU_ON: MMU off, x0 = the context the boot
 * CPU passed (unused: it left our stack and per-CPU block in
 * secondary_stack/secondary_cpu, written to memory with the caches
 * cleaned). */
    .globl secondary_entry
secondary_entry:
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    1f
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1
    mov     x1, #0x3c5
    msr     spsr_el2, x1
    adr     x1, 1f
    msr     elr_el2, x1
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    eret
1:  msr     daifset, #0xf
    msr     spsel, #1
    bl      mmu_on
    ldr     x1, =2f
    br      x1
2:  msr     ttbr0_el1, xzr
    tlbi    vmalle1
    dsb     nsh
    isb
    ldr     x1, =secondary_stack
    ldr     x1, [x1]
    mov     sp, x1
    ldr     x1, =vectors
    msr     vbar_el1, x1
    ldr     x0, =secondary_cpu
    ldr     x0, [x0]
    msr     tpidr_el1, x0
    bl      aarch64_secondary_main
3:  wfi
    b       3b

    .section .bss.pagetables, "aw", @nobits
    .balign 4096
    .globl boot_l0, boot_l1
boot_l0:        .space 4096
boot_l0_ident:  .space 4096
boot_l1:        .space 4096

    .section .bss
    .balign 16
    .globl stack_top
    .space 16384
stack_top:
