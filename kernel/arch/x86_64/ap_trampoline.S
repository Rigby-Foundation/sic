/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* AP startup trampoline. Copied to physical 0x8000 by smp.c and entered in
 * real mode by a SIPI. Every absolute address below is computed relative to
 * the copy at 0x8000, so this code must not be linked with -fpic tricks.
 *
 * Parameters (filled in by the BSP before the SIPI) live at the end. */

.set TRAMPOLINE_BASE, 0x8000
#define ABS(sym) (sym - ap_trampoline_start + TRAMPOLINE_BASE)

.section .rodata
.global ap_trampoline_start, ap_trampoline_end
.global ap_param_cr3, ap_param_stack, ap_param_entry, ap_param_cpu

.code16
ap_trampoline_start:
        cli
        cld
        ljmp    $0, $ABS(1f)            /* normalise CS to 0 */
1:
        xor     %ax, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss

        lgdtl   ABS(gdt_ptr)
        mov     %cr0, %eax
        or      $1, %eax
        mov     %eax, %cr0
        ljmpl   $0x08, $ABS(pm32)

.code32
pm32:
        mov     $0x10, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss

        mov     %cr4, %eax
        or      $(1 << 5), %eax         /* PAE */
        mov     %eax, %cr4

        mov     ABS(ap_param_cr3), %eax
        mov     %eax, %cr3

        mov     $0xC0000080, %ecx       /* EFER: LME | NXE */
        rdmsr
        or      $((1 << 8) | (1 << 11)), %eax
        wrmsr

        mov     %cr0, %eax
        or      $0x80000001, %eax       /* PG | PE */
        mov     %eax, %cr0

        ljmp    $0x18, $ABS(lm64)

.code64
lm64:
        mov     $0x10, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss
        xor     %ax, %ax
        mov     %ax, %fs
        mov     %ax, %gs

        mov     ABS(ap_param_stack), %rsp
        mov     ABS(ap_param_cpu), %rdi
        mov     ABS(ap_param_entry), %rax
        call    *%rax
3:      cli
        hlt
        jmp     3b

.align 16
gdt:
        .quad   0
        .quad   0x00CF9A000000FFFF      /* 0x08: 32-bit code */
        .quad   0x00CF92000000FFFF      /* 0x10: 32-bit data */
        .quad   0x00209A0000000000      /* 0x18: 64-bit code */
gdt_ptr:
        .word   gdt_ptr - gdt - 1
        .long   ABS(gdt)

.align 8
ap_param_cr3:   .quad 0
ap_param_stack: .quad 0
ap_param_entry: .quad 0
ap_param_cpu:   .quad 0
ap_trampoline_end:
