/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* TrustZone calls, for the GPU's zap shader and the modem's firmware. */
#include "asm/qcom_scm.h"
#include "proc/sched.h"
#include "printf.h"

/* A Qualcomm SCM call: SMCCC standard call, SMC64, owner SIP (2). A long
 * one comes back "interrupted" (1) so the normal world can take its
 * interrupts; it is resumed with function id 1 and the context TrustZone
 * left in x6 (Linux: ARM_SMCCC_QUIRK_QCOM_A6), for as long as it says so. */
struct scm_res qcom_scm_call(uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t fn = 0x42000000u | svc << 8 | cmd, a6 = 0;
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1"), x2 __asm__("x2"), x3 __asm__("x3");
    register uint64_t x4 __asm__("x4"), x5 __asm__("x5"), x6 __asm__("x6");
    uint32_t resumes = 0, busy = 0;
    for (;;) {
        x0 = fn; x1 = arginfo; x2 = a; x3 = b; x4 = c; x5 = 0; x6 = a6;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : : "x7", "x8", "x9",
                         "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        a6 = x6;
        if (x0 == 1) {                                                      /* interrupted: resume */
            fn = 1;
            if (++resumes % 10000 == 0) kprintf("scm: %x/%x: %u resumes\n", svc, cmd, resumes);
            continue;
        }
        if ((int64_t)x0 == -12 && ++busy < 100) { task_sleep_ms(30); continue; }   /* busy: again */
        break;
    }
    if (resumes) kprintf("scm: %x/%x: done after %u resumes\n", svc, cmd, resumes);
    return (struct scm_res){ x0, x1, x2, x3 };
}
