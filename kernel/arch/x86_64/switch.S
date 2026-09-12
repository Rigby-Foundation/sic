/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 *
 * Saves the callee-saved registers of the current task on its own stack,
 * stores the stack pointer into *old_rsp, and resumes the other task by
 * loading its stack pointer and popping the same frame. A brand-new task's
 * stack is built by the scheduler to look like it was saved here, with
 * task_trampoline as the return address. */

.section .text
.code64

.global switch_context
switch_context:
        push    %rbp
        push    %rbx
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        mov     %rsp, (%rdi)
        mov     %rsi, %rsp
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbx
        pop     %rbp
        ret

/* First code a new task runs. r12 = arg, r13 = entry (set up by task_create).
 * We arrive holding the scheduler lock with interrupts off, like anyone
 * returning from switch_context. */
.global task_trampoline
task_trampoline:
        call    sched_unlock_new_task
        sti
        mov     %r12, %rdi
        call    *%r13
        call    task_exit
        ud2
