/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Exception stubs. Each pushes (error code if CPU didn't), vector, then jumps to common. */

.section .text
.code64

.macro ISR_NOERR n
.global isr\n
isr\n:
        pushq   $0
        pushq   $\n
        jmp     isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
        pushq   $\n
        jmp     isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* IRQs 0-15, remapped to vectors 32-47 */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
ISR_NOERR 240                          /* TLB shootdown IPI */
ISR_NOERR 255                          /* LAPIC spurious */

isr_common:
        testl   $3, 24(%rsp)            /* CS.RPL: did we come from ring 3? */
        jz      1f
        swapgs
1:      push    %rax
        push    %rbx
        push    %rcx
        push    %rdx
        push    %rsi
        push    %rdi
        push    %rbp
        push    %r8
        push    %r9
        push    %r10
        push    %r11
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, %rdi
        call    isr_handler
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %r11
        pop     %r10
        pop     %r9
        pop     %r8
        pop     %rbp
        pop     %rdi
        pop     %rsi
        pop     %rdx
        pop     %rcx
        pop     %rbx
        pop     %rax
        add     $16, %rsp               /* vector + error code */
        testl   $3, 8(%rsp)
        jz      2f
        swapgs
2:      iretq

.global idt_load
idt_load:
        lidt    (%rdi)
        ret
