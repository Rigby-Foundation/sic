/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* TrustZone calls, for the GPU's zap shader and the modem's firmware. */
#include "asm/qcom_scm.h"
#include "proc/sched.h"
#include "printf.h"
#include "string.h"
#include "mm/pmm.h"
#include "asm/memlayout.h"

/* A Qualcomm SCM call: SMCCC standard call, SMC64, owner SIP (2). A long
 * one comes back "interrupted" (1) so the normal world can take its
 * interrupts; it is resumed with function id 1 and the context TrustZone
 * left in x6 (Linux: ARM_SMCCC_QUIRK_QCOM_A6), for as long as it says so. */
static struct scm_res scm_raw(uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    const uint64_t fn0 = 0x42000000u | svc << 8 | cmd;
    uint64_t fn = fn0, a6 = 0;
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1"), x2 __asm__("x2"), x3 __asm__("x3");
    register uint64_t x4 __asm__("x4"), x5 __asm__("x5"), x6 __asm__("x6");
    uint32_t resumes = 0, busy = 0;
    for (;;) {
        x0 = fn; x1 = arginfo; x2 = a; x3 = b; x4 = c; x5 = d; x6 = a6;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6) : : "x7", "x8", "x9",
                         "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        a6 = x6;
        if (x0 == 1) {                                                      /* interrupted: resume */
            fn = 1;
            if (++resumes % 10000 == 0) kprintf("scm: %x/%x: %u resumes\n", svc, cmd, resumes);
            continue;
        }
        if ((int64_t)x0 == -12 && ++busy < 100) { fn = fn0; a6 = 0; task_sleep_ms(30); continue; }   /* busy: again */
        break;
    }
    if (resumes) kprintf("scm: %x/%x: done after %u resumes\n", svc, cmd, resumes);
    return (struct scm_res){ x0, x1, x2, x3 };
}

struct scm_res qcom_scm_call(uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c)
{
    return scm_raw(svc, cmd, arginfo, a, b, c, 0);
}

/* Memory TrustZone reads by its physical address: written back to the
 * point of coherency, so it sees what the CPU wrote. */
void qcom_dma_clean(const void *p, size_t len)
{
    for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* More than four arguments: the first three in registers, the rest in a
 * buffer whose address goes in the fourth (Linux's SCM_SMC_FIRST_EXT_IDX). */
struct scm_res qcom_scm_call_n(uint32_t svc, uint32_t cmd, uint32_t arginfo, const uint64_t *args, int n)
{
    if (n <= 4) return scm_raw(svc, cmd, arginfo, n > 0 ? args[0] : 0, n > 1 ? args[1] : 0, n > 2 ? args[2] : 0, n > 3 ? args[3] : 0);
    uint64_t page = pmm_alloc_page();
    if (!page) return (struct scm_res){ (uint64_t)-12, 0, 0, 0 };
    uint64_t *ext = P2V(page);
    for (int i = 3; i < n; i++) ext[i - 3] = args[i];
    qcom_dma_clean(ext, (size_t)(n - 3) * 8);
    struct scm_res r = scm_raw(svc, cmd, arginfo, args[0], args[1], args[2], page);
    pmm_free_page(page);
    return r;
}

/* Hand memory from the apps processor (HLOS) to other execution
 * environments (the modem's VMID 15, say), with these permissions, as
 * Linux's qcom_scm_assign_mem: 0 when TrustZone agrees. */
int qcom_scm_assign_mem(uint64_t phys, uint64_t size, const uint32_t *vmids, const uint32_t *perms, int n)
{
    uint64_t page = pmm_alloc_page();
    if (!page || n < 1 || n > 8) return -1;
    uint8_t *b = P2V(page);
    memset(b, 0, PAGE_SIZE);
    uint64_t mem[2] = { phys, size };
    memcpy(b, mem, 16);                                         /* {addr, size} */
    uint32_t src = SCM_VMID_HLOS;
    memcpy(b + 64, &src, 4);                                    /* the current owner */
    for (int i = 0; i < n; i++) {                               /* {vmid, perm, ctx, ctx_size, unused} */
        uint32_t d[6] = { vmids[i], perms[i], 0, 0, 0, 0 };
        memcpy(b + 128 + 24 * i, d, 24);
    }
    qcom_dma_clean(b, PAGE_SIZE);
    const uint64_t args[7] = { page, 16, page + 64, 4, page + 128, 24u * (uint64_t)n, 0 };
    struct scm_res r = qcom_scm_call_n(SCM_SVC_MP, SCM_MP_ASSIGN,
                                       SCM_ARGS7(SCM_ARG_RO, 0, SCM_ARG_RO, 0, SCM_ARG_RO, 0, 0), args, 7);
    pmm_free_page(page);
    if (r.a0 || r.a1) { kprintf("scm: assign %lx+%lx: %ld/%lu\n", (unsigned long)phys, (unsigned long)size, (long)r.a0, (unsigned long)r.a1); return -1; }
    return 0;
}
