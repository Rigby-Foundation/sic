/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* syscall instruction entry. On entry: rcx = user rip, r11 = user rflags,
 * interrupts masked by SFMASK, still on the user stack, user GS. */

.section .text
.code64
.global syscall_entry
syscall_entry:
        swapgs
        mov     %rsp, %gs:16            /* stash user rsp */
        mov     %gs:8, %rsp             /* current task's kernel stack */

        /* Build an iretq-shaped frame so the C side sees a uniform struct. */
        pushq   $0x1B                   /* ss = SEL_UDATA | 3 */
        pushq   %gs:16                  /* user rsp */
        push    %r11                    /* rflags */
        pushq   $0x23                   /* cs = SEL_UCODE | 3 */
        push    %rcx                    /* rip */

        push    %rax
        push    %rdi
        push    %rsi
        push    %rdx
        push    %r10
        push    %r8
        push    %r9
        push    %rbx
        push    %rbp
        push    %r12
        push    %r13
        push    %r14
        push    %r15

        sti
        mov     %rsp, %rdi
        call    syscall_dispatch
        cli

/* Return to user mode from a struct syscall_frame at %rsp. Also the entry
 * point for a forked child, which gets a copied frame with rax = 0. */
.global syscall_return
syscall_return:
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbp
        pop     %rbx
        pop     %r9
        pop     %r8
        pop     %r10
        pop     %rdx
        pop     %rsi
        pop     %rdi
        pop     %rax

        pop     %rcx                    /* rip */
        add     $8, %rsp                /* cs */
        pop     %r11                    /* rflags */
        pop     %rsp                    /* back onto the user stack */
        swapgs
        sysretq

/* void fork_return(struct syscall_frame *f): never returns. */
.global fork_return
fork_return:
        cli
        mov     %rdi, %rsp
        jmp     syscall_return

/* void enter_usermode(rip, rsp, argc, argv): first drop into ring 3, with
 * argc/argv in rdi/rsi for crt0. */
.global enter_usermode
enter_usermode:
        cli
        swapgs
        pushq   $0x1B                   /* ss */
        push    %rsi                    /* rsp */
        pushq   $0x202                  /* rflags: IF */
        pushq   $0x23                   /* cs */
        push    %rdi                    /* rip */
        mov     %rdx, %rdi              /* argc */
        mov     %rcx, %rsi              /* argv */
        xor     %eax, %eax
        xor     %ebx, %ebx
        xor     %ecx, %ecx
        xor     %edx, %edx
        xor     %ebp, %ebp
        xor     %r8d, %r8d
        xor     %r9d, %r9d
        xor     %r10d, %r10d
        xor     %r11d, %r11d
        xor     %r12d, %r12d
        xor     %r13d, %r13d
        xor     %r14d, %r14d
        xor     %r15d, %r15d
        iretq
