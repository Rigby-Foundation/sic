/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The Adreno 610 of a Qualcomm "khaje" phone (SM6225), the GPU without a
 * GMU: the kernel powers it itself, through the GPU clock controller
 * (GPUCC) and the global one (GCC). The sequence follows what Linux's
 * gpucc-sm6115 and gdsc drivers do for the same block on bengal:
 *   1. GCC: let GPLL0 through to the GPU, and the GPU's bus clocks;
 *   2. the CX power domain (GDSC), the GPU's AHB/CXO clocks;
 *   3. the core clock's source: GPLL0 / 2, 300 MHz (no GPU PLL needed);
 *   4. the GX power domain: reset pulse, IO clamp off, power;
 *   5. the core clock.
 * Then the command processor (CP): its microcode (a630_sqe.fw), a ring
 * buffer, CP_ME_INIT; and the zap shader, which TrustZone checks and loads
 * (a610_zap.mdt + .b02, PAS id 13) so the CP may leave secure mode; then
 * the 2D engine fills rectangles (Mesa turnip's r2d clear). The GPU sees
 * memory through its SMMU and our page table: only what the kernel maps.
 * /dev/adreno: write "on" to power it, "start" for all of it; read for its
 * state (and its SMMU's). */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "proc/syscall.h"
#include "asm/timer.h"
#include "abi/adreno.h"
#include "asm/qcom_scm.h"

#define GCC         0x1400000
#define GPUCC       0x5990000
#define GPU         0x5900000
#define GPU_SMMU    0x59a0000

/* GCC */
#define GCC_GPU_CFG_AHB     0x36004
#define GCC_GPU_MEMNOC_GFX  0x3600c
#define GCC_GPU_SNOC_DVM    0x36018
#define GCC_BIMC_GPU_AXI    0x71154
#define GCC_VOTE            0x79004         /* bit 15: GPLL0 to the GPU, bit 16: its /2 */

/* GPUCC */
#define CC_GX_BCR           0x1008
#define CC_GX_GDSCR         0x100c
#define CC_GFX3D_CMD_RCGR   0x101c
#define CC_GFX3D_CFG_RCGR   0x1020
#define CC_GX_GFX3D_CBCR    0x1054
#define CC_GX_CXO_CBCR      0x1060
#define CC_CX_GDSCR         0x106c
#define CC_AHB_CBCR         0x1078
#define CC_CXO_CBCR         0x109c
#define CC_GX_CLAMP_IO      0x1508
#define CC_SMMU_VOTE_CBCR   0x5000

#define CBCR_OFF    (1u << 31)
#define GDSC_PWR_ON (1u << 31)
#define GDSC_COLLAPSE 1u

/* the GPU's registers (dword offsets, as in Linux's a6xx.xml) */
#define RBBM_GBIF_CLIENT_QOS_CNTL 0x11
#define RBBM_GBIF_HALT      0x16
#define RBBM_INTERFACE_HANG_INT_CNTL 0x1f
#define RBBM_INT_0_MASK     0x38
#define RBBM_INT_0_STATUS   0x201
#define RBBM_STATUS         0x210
#define RBBM_PERFCTR_CNTL   0x500
#define RBBM_PERFCTR_GPU_BUSY_MASKED 0x50b
#define CP_RB_BASE          0x800       /* 64-bit */
#define CP_RB_CNTL          0x802
#define CP_RB_RPTR          0x806
#define CP_RB_WPTR          0x807
#define CP_SQE_CNTL         0x808
#define CP_HW_FAULT         0x821
#define CP_INTERRUPT_STATUS 0x823
#define CP_PROTECT_STATUS   0x824
#define CP_SQE_INSTR_BASE   0x830       /* 64-bit */
#define CP_ADDR_MODE_CNTL   0x842
#define CP_PROTECT_CNTL     0x84f
#define CP_PROTECT(n)       (0x850 + (n))
#define CP_ROQ_THRESHOLDS_1 0x8c1
#define CP_ROQ_THRESHOLDS_2 0x8c2
#define CP_MEM_POOL_SIZE    0x8c3
#define CP_PERFCTR_CP_SEL0  0x8d0
#define CP_MEM_POOL_DBG_ADDR 0x90e
#define CP_AHB_CNTL         0x98d
#define VSC_ADDR_MODE_CNTL  0xc01
#define UCHE_ADDR_MODE_CNTL 0xe00
#define UCHE_WRITE_RANGE_MAX 0xe05      /* 64-bit, and the next two */
#define UCHE_WRITE_THRU_BASE 0xe07
#define UCHE_TRAP_BASE      0xe09
#define UCHE_GMEM_RANGE_MIN 0xe0b
#define UCHE_GMEM_RANGE_MAX 0xe0d
#define UCHE_CACHE_WAYS     0xe17
#define UCHE_FILTER_CNTL    0xe18
#define UCHE_CLIENT_PF      0xe19
#define GBIF_QSB_SIDE(n)    (0x3c03 + (n))
#define GBIF_HALT           0x3c45
#define GRAS_ADDR_MODE_CNTL 0x8601
#define RB_ADDR_MODE_CNTL   0x8e05
#define VPC_ADDR_MODE_CNTL  0x9601
#define PC_DBG_ECO_CNTL     0x9e00
#define PC_ADDR_MODE_CNTL   0x9e01
#define VFD_ADDR_MODE_CNTL  0xa601
#define SP_ADDR_MODE_CNTL   0xae01
#define TPL1_ADDR_MODE_CNTL 0xb601
#define HLSQ_ADDR_MODE_CNTL 0xbe05
#define RBBM_SECVID_TRUST_CNTL 0xf400
#define RBBM_SECVID_TSB_TRUSTED_BASE 0xf800   /* 64-bit */
#define RBBM_SECVID_TSB_TRUSTED_SIZE 0xf802
#define RBBM_SECVID_TSB_CNTL 0xf803
#define RBBM_SECVID_TSB_ADDR_MODE_CNTL 0xf810

/* the 2D engine (Mesa's turnip r2d_* path) */
#define GRAS_A2D_BLT_CNTL   0x8400
#define GRAS_A2D_DEST_TL    0x8405
#define GRAS_A2D_DEST_BR    0x8406
#define RB_A2D_BLT_CNTL     0x8c00
#define RB_A2D_PIXEL_CNTL   0x8c01
#define RB_A2D_DEST_BUFFER_INFO 0x8c17  /* then BASE (64-bit) and PITCH (bytes >> 6) */
#define RB_A2D_CLEAR_COLOR_DW0 0x8c2c
#define RB_RBP_CNTL         0x8e01
#define RB_DBG_ECO_CNTL     0x8e04
#define RB_CCU_CNTL         0x8e07
#define SP_A2D_OUTPUT_INFO  0xacc0
#define FMT6_8_8_8_8_UNORM  0x30
#define SWAP_WXYZ           1           /* B8G8R8A8 in memory: the splash's XRGB8888 */

/* PM4 type 7 packets: the CP's commands */
#define CP_NOP              0x10
#define CP_WAIT_FOR_IDLE    0x26
#define CP_BLIT             0x2c
#define CP_EVENT_WRITE      0x46
#define PC_CCU_FLUSH_COLOR_TS 0x1d
#define CACHE_FLUSH_TS      0x04
#define CP_MEM_WRITE        0x3d
#define CP_ME_INIT          0x48
#define CP_SET_SECURE_MODE  0x66
#define CP_INDIRECT_BUFFER  0x3f
#define CP_SET_PROTECTED_MODE 0x5f

#define RING_BYTES  0x8000
#define GPU_PAS_ID  13

static volatile uint8_t *gcc, *cc, *gpu, *smmu;
static int powered;

static uint32_t rd(volatile uint8_t *b, uint32_t off) { return mmio_read32(b + off); }
static void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { mmio_write32(b + off, v); }
uint32_t adreno_read(uint32_t reg) { return mmio_read32(gpu + reg * 4); }
void adreno_write(uint32_t reg, uint32_t v) { mmio_write32(gpu + reg * 4, v); }
static void adreno_write64(uint32_t reg, uint64_t v) { adreno_write(reg, (uint32_t)v); adreno_write(reg + 1, (uint32_t)(v >> 32)); }

/* A branch clock on, and running within a millisecond (or not: some say
 * "off" until something downstream wakes). */
static int clk_on(volatile uint8_t *b, uint32_t cbcr)
{
    wr(b, cbcr, rd(b, cbcr) | 1);
    for (int i = 0; i < 100; i++) {
        if (!(rd(b, cbcr) & CBCR_OFF)) return 0;
        task_sleep_ms(0);
    }
    return -1;
}

static int gdsc_on(uint32_t gdscr)
{
    wr(cc, gdscr, rd(cc, gdscr) & ~GDSC_COLLAPSE);
    for (int i = 0; i < 100; i++) {
        if (rd(cc, gdscr) & GDSC_PWR_ON) return 0;
        task_sleep_ms(1);
    }
    return -1;
}

int adreno_power_on(void)
{
    if (powered) return 0;
    wr(gcc, GCC_VOTE, rd(gcc, GCC_VOTE) | 3u << 15);
    clk_on(gcc, GCC_GPU_CFG_AHB);
    wr(gcc, GCC_GPU_MEMNOC_GFX, rd(gcc, GCC_GPU_MEMNOC_GFX) | 1);  /* these run once the domains are up */
    wr(gcc, GCC_GPU_SNOC_DVM, rd(gcc, GCC_GPU_SNOC_DVM) | 1);
    wr(gcc, GCC_BIMC_GPU_AXI, rd(gcc, GCC_BIMC_GPU_AXI) | 1);

    if (gdsc_on(CC_CX_GDSCR) != 0) { kprintf("adreno: the CX power domain did not come up\n"); return -1; }
    clk_on(cc, CC_AHB_CBCR);
    clk_on(cc, CC_CXO_CBCR);
    clk_on(cc, CC_SMMU_VOTE_CBCR);

    /* the core clock: GPLL0 (source 5) divided by 2 (the field is 2 * div - 1) */
    wr(cc, CC_GFX3D_CFG_RCGR, 5u << 8 | 3);
    wr(cc, CC_GFX3D_CMD_RCGR, rd(cc, CC_GFX3D_CMD_RCGR) | 1);
    for (int i = 0; i < 1000 && (rd(cc, CC_GFX3D_CMD_RCGR) & 1); i++) ;

    wr(cc, CC_GX_BCR, 1);
    task_sleep_ms(1);
    wr(cc, CC_GX_BCR, 0);
    wr(cc, CC_GX_CLAMP_IO, rd(cc, CC_GX_CLAMP_IO) & ~1u);
    if (gdsc_on(CC_GX_GDSCR) != 0) { kprintf("adreno: the GX power domain did not come up\n"); return -1; }
    task_sleep_ms(1);
    clk_on(cc, CC_GX_CXO_CBCR);
    if (clk_on(cc, CC_GX_GFX3D_CBCR) != 0) { kprintf("adreno: the core clock does not run\n"); return -1; }
    clk_on(gcc, GCC_GPU_MEMNOC_GFX);
    powered = 1;
    kprintf("adreno: powered (core clock 300 MHz), RBBM_STATUS %08x\n", adreno_read(RBBM_STATUS));
    return 0;
}

/* ---- memory the GPU sees -------------------------------------------------------------- */

/* The GPU's own SMMU: its stream (ids 0 and 1) goes through context bank 0
 * and our page table (LPAE stage 1, 4 KiB pages, 39-bit addresses, three
 * levels). The GPU reaches only what is mapped: everything the kernel gives
 * it, at its physical address (all below 4 GiB, as the A610 wants), the
 * frame buffer too. (S2CR "bypass" is no option: this firmware turns it into
 * "fault".) Page tables are uncached memory: the SMMU walks without snooping. */
#define SMMU_PAGE       0x1000
#define SMMU_CB(n)      (smmu + (8 + (n)) * SMMU_PAGE)   /* after 8 pages of global space (IDR1) */
#define CB_SCTLR        0x00
#define CB_TCR2         0x10
#define CB_TTBR0        0x20
#define CB_TCR          0x30
#define CB_MAIR0        0x38
#define CB_FSR          0x58
#define CB_FAR          0x60
#define CB_TLBIASID     0x610
#define CB_TLBSYNC      0x7f0
#define CB_TLBSTATUS    0x7f4
#define PTE_TABLE       3ull
#define PTE_PAGE        (3ull | 1ull << 10 | 2ull << 8 | 1ull << 6)   /* page, accessed, outer shareable, RW any level, MAIR 0 (NC) */

static volatile uint64_t *pt_l1;
static uint64_t pt_l1_phys;

/* Memory used through an uncached mapping is also in the cacheable direct
 * map: lines left there (dirty ones would be written back over what the GPU
 * or the SMMU reads) go first. */
static void flush_alias(uint64_t phys, size_t len)
{
    for (uint64_t a = (uint64_t)(uintptr_t)P2V(phys) & ~63ull; a < (uint64_t)(uintptr_t)P2V(phys) + len; a += 64)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static volatile uint64_t *pt_table_alloc(uint64_t *phys)
{
    *phys = pmm_alloc_page();
    if (!*phys) return NULL;
    flush_alias(*phys, PAGE_SIZE);
    volatile uint64_t *t = vmm_map_wc(*phys, PAGE_SIZE);
    if (t) for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* The next level's table under entry e of t, made if it is missing. */
static volatile uint64_t *pt_next(volatile uint64_t *t, int e)
{
    static struct { uint64_t phys; volatile uint64_t *virt; } known[64];
    static int nknown;
    uint64_t phys;
    if (t[e] & 1) {
        phys = t[e] & 0x0000fffffffff000ull;
        for (int i = 0; i < nknown; i++) if (known[i].phys == phys) return known[i].virt;
        return NULL;
    }
    if (nknown == 64) return NULL;
    volatile uint64_t *n = pt_table_alloc(&phys);
    if (!n) return NULL;
    known[nknown].phys = phys; known[nknown++].virt = n;
    __asm__ volatile("dsb st" ::: "memory");
    t[e] = phys | PTE_TABLE;
    return n;
}

static void tlb_flush(void)
{
    volatile uint8_t *cb = SMMU_CB(0);
    wr(cb, CB_TLBIASID, 0);
    wr(cb, CB_TLBSYNC, 0);
    for (int i = 0; i < 100000 && (rd(cb, CB_TLBSTATUS) & 1); i++) ;
}

/* [phys, phys + size) at the same GPU address. */
static int gpu_map(uint64_t phys, uint64_t size)
{
    for (uint64_t a = phys & ~0xfffull; a < phys + size; a += PAGE_SIZE) {
        volatile uint64_t *l2 = pt_next(pt_l1, (int)(a >> 30 & 511));
        volatile uint64_t *l3 = l2 ? pt_next(l2, (int)(a >> 21 & 511)) : NULL;
        if (!l3) return -1;
        l3[a >> 12 & 511] = a | PTE_PAGE;
    }
    __asm__ volatile("dsb st" ::: "memory");
    tlb_flush();
    return 0;
}

static void gpu_unmap(uint64_t phys, uint64_t size)
{
    for (uint64_t a = phys & ~0xfffull; a < phys + size; a += PAGE_SIZE) {
        volatile uint64_t *l2 = (pt_l1[a >> 30 & 511] & 1) ? pt_next(pt_l1, (int)(a >> 30 & 511)) : NULL;
        volatile uint64_t *l3 = (l2 && (l2[a >> 21 & 511] & 1)) ? pt_next(l2, (int)(a >> 21 & 511)) : NULL;
        if (l3) l3[a >> 12 & 511] = 0;
    }
    __asm__ volatile("dsb st" ::: "memory");
    tlb_flush();
}

static int gpu_vm_init(void)
{
    pt_l1 = pt_table_alloc(&pt_l1_phys);
    if (!pt_l1) return -1;
    volatile uint8_t *cb = SMMU_CB(0);
    wr(cb, CB_SCTLR, 0);
    wr(smmu, SMMU_PAGE + 0x800 + 4 * 0, 1);                 /* CBA2R0: AArch64 tables */
    wr(smmu, SMMU_PAGE + 4 * 0, 1u << 16 | 3u << 8 | 0xfu << 12);   /* CBAR0: stage 1 (stage 2 bypass), weakest attributes */
    wr(cb, CB_TCR2, 2u | 7u << 15);                         /* 40-bit physical addresses, SEP upstream */
    wr(cb, CB_TCR, 25u | 2u << 12 | 1u << 23);              /* T0SZ 25 (39 bits), 4 KiB, uncached walks, outer shareable; no TTBR1 */
    wr(cb, CB_TTBR0, (uint32_t)pt_l1_phys);
    wr(cb, CB_TTBR0 + 4, (uint32_t)(pt_l1_phys >> 32));     /* ASID 0 */
    wr(cb, CB_MAIR0, 0x0004ff44);                           /* 0: normal uncached, 1: write-back, 2: device */
    wr(cb, CB_FSR, rd(cb, CB_FSR));                         /* stale faults off */
    tlb_flush();
    wr(cb, CB_SCTLR, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 6);   /* on, TEX remap, AF, faults: abort and report */
    wr(smmu, 0xc00, 0);                                     /* S2CR0: translate, context bank 0 */
    wr(smmu, 0x800, 1u << 31 | 1u << 16 | 0);               /* SMR0: valid, id 0, mask 1 */
    return 0;
}

/* Pages below 4 GiB, mapped uncached for the CPU (the GPU does not snoop)
 * and at their physical address for the GPU. */
static void *gpu_alloc(size_t bytes, uint64_t *phys)
{
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    *phys = pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!*phys) return NULL;
    flush_alias(*phys, pages * PAGE_SIZE);
    void *v = vmm_map_wc(*phys, pages * PAGE_SIZE);
    if (v) memset(v, 0, pages * PAGE_SIZE);
    if (v && gpu_map(*phys, pages * PAGE_SIZE) != 0) return NULL;
    return v;
}

/* ---- the command processor ------------------------------------------------------------- */

static volatile uint32_t *ring, *memstore;
static uint64_t ring_phys, memstore_phys, sqe_phys;
static uint32_t wptr;
static int started;

static uint32_t parity(uint32_t v)
{
    return (0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28))) & 1;
}

static void flush(void);
/* A dword into the ring; when it is full, the CP is let catch up first. */
static void out(uint32_t v)
{
    uint32_t next = (wptr + 1) % (RING_BYTES / 4);
    if (next == adreno_read(CP_RB_RPTR)) {
        flush();
        uint64_t t0 = timer_ms();
        while (next == adreno_read(CP_RB_RPTR))
            if (timer_ms() - t0 > 2000) { kprintf("adreno: the CP is stuck (rptr %u, RBBM_STATUS %08x)\n", adreno_read(CP_RB_RPTR), adreno_read(RBBM_STATUS)); t0 = timer_ms(); }
    }
    ring[wptr] = v;
    wptr = next;
}
static void out_pkt7(uint32_t op, uint32_t cnt) { out(0x70000000u | cnt | parity(cnt) << 15 | (op & 0x7f) << 16 | parity(op) << 23); }
static void out_pkt4(uint32_t reg, uint32_t cnt) { out(0x40000000u | cnt | parity(cnt) << 7 | (reg & 0x3ffff) << 8 | parity(reg) << 27); }
static void out_reg(uint32_t reg, uint32_t v) { out_pkt4(reg, 1); out(v); }

static void flush(void)
{
    __asm__ volatile("dsb st" ::: "memory");
    adreno_write(CP_RB_WPTR, wptr);
}

/* The CP has read all we wrote and the GPU is idle (the CP's AHB master
 * aside); -1 after a second, with what it looked like. */
static int idle(const char *what)
{
    for (int i = 0; i < 1000; i++) {
        if (adreno_read(CP_RB_RPTR) == wptr && !(adreno_read(RBBM_STATUS) & ~1u)) return 0;
        task_sleep_ms(1);
    }
    kprintf("adreno: %s: no idle: rptr %u wptr %u status %08x int %08x hw fault %08x cp int %08x protect %08x\n", what,
            adreno_read(CP_RB_RPTR), wptr, adreno_read(RBBM_STATUS), adreno_read(RBBM_INT_0_STATUS),
            adreno_read(CP_HW_FAULT), adreno_read(CP_INTERRUPT_STATUS), adreno_read(CP_PROTECT_STATUS));
    return -1;
}

/* Linux's a630 register protection list (a6xx_catalog.c), entry = reg | len << 18 | no-read << 31 */
#define PROT_RO(r, l) ((uint32_t)(l) << 18 | (r))
#define PROT_NO(r, l) (1u << 31 | (uint32_t)(l) << 18 | (r))
static const uint32_t protect[] = {
    PROT_RO(0x00000, 0x04ff), PROT_RO(0x00501, 0x0005), PROT_RO(0x0050b, 0x02f4), PROT_NO(0x0050e, 0),
    PROT_NO(0x00510, 0), PROT_NO(0x00534, 0), PROT_NO(0x00800, 0x0082), PROT_NO(0x008a0, 0x0008),
    PROT_NO(0x008ab, 0x0024), PROT_RO(0x008de, 0x00ae), PROT_NO(0x00900, 0x004d), PROT_NO(0x0098d, 0x0272),
    PROT_NO(0x00e00, 0x0001), PROT_NO(0x00e03, 0x000c), PROT_NO(0x03c00, 0x00c3), PROT_RO(0x03cc4, 0x1fff),
    PROT_NO(0x08630, 0x01cf), PROT_NO(0x08e00, 0), PROT_NO(0x08e08, 0), PROT_NO(0x08e50, 0x001f),
    PROT_NO(0x09624, 0x01db), PROT_NO(0x09e70, 0x0001), PROT_NO(0x09e78, 0x0187), PROT_NO(0x0a630, 0x01cf),
    PROT_NO(0x0ae02, 0), PROT_NO(0x0ae50, 0x032f), PROT_NO(0x0b604, 0), PROT_NO(0x0be02, 0x0001),
    PROT_NO(0x0be20, 0x17df), PROT_NO(0x0f000, 0x0bff), PROT_RO(0x0fc00, 0x1fff), PROT_NO(0x11c00, 0),
};

/* What Linux's a6xx hw_init writes for an A610 (clock gating left off). */
static void hw_init(void)
{
    adreno_write(GBIF_HALT, 0); adreno_read(GBIF_HALT);
    adreno_write(RBBM_GBIF_HALT, 0); adreno_read(RBBM_GBIF_HALT);
    adreno_write(RBBM_SECVID_TSB_CNTL, 0);
    adreno_write64(RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    adreno_write(RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
    static const uint16_t addr_mode[] = { CP_ADDR_MODE_CNTL, VSC_ADDR_MODE_CNTL, GRAS_ADDR_MODE_CNTL, RB_ADDR_MODE_CNTL,
        PC_ADDR_MODE_CNTL, HLSQ_ADDR_MODE_CNTL, VFD_ADDR_MODE_CNTL, VPC_ADDR_MODE_CNTL, UCHE_ADDR_MODE_CNTL,
        SP_ADDR_MODE_CNTL, TPL1_ADDR_MODE_CNTL, RBBM_SECVID_TSB_ADDR_MODE_CNTL };
    for (size_t i = 0; i < sizeof addr_mode / sizeof addr_mode[0]; i++) adreno_write(addr_mode[i], 1);   /* 64-bit addresses */
    for (int i = 0; i < 4; i++) adreno_write(GBIF_QSB_SIDE(i), 0x00071620);
    adreno_write(RBBM_GBIF_CLIENT_QOS_CNTL, 3);
    adreno_write(RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
    uint64_t trap = 0x1fffffffff000ULL;
    adreno_write64(UCHE_WRITE_RANGE_MAX, trap + 0xfc0);
    adreno_write64(UCHE_TRAP_BASE, trap);
    adreno_write64(UCHE_WRITE_THRU_BASE, trap);
    adreno_write64(UCHE_GMEM_RANGE_MIN, 0x100000);                     /* GMEM: 132 KiB at 1 MiB */
    adreno_write64(UCHE_GMEM_RANGE_MAX, 0x100000 + 0x21000 - 1);
    adreno_write(UCHE_FILTER_CNTL, 0x804);
    adreno_write(UCHE_CACHE_WAYS, 4);
    adreno_write(CP_ROQ_THRESHOLDS_2, 0x00800060);
    adreno_write(CP_ROQ_THRESHOLDS_1, 0x40201b16);
    adreno_write(CP_MEM_POOL_SIZE, 48);
    adreno_write(CP_MEM_POOL_DBG_ADDR, 47);
    adreno_write(PC_DBG_ECO_CNTL, 0x00080000);
    adreno_write(CP_AHB_CNTL, 1);
    adreno_write(RBBM_PERFCTR_CNTL, 1);
    adreno_write(CP_PERFCTR_CP_SEL0, 0);
    adreno_write(RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x3ffff);
    adreno_write(UCHE_CLIENT_PF, 1u << 7 | 1);
    adreno_write(CP_PROTECT_CNTL, 1u << 0 | 1u << 1 | 1u << 3);        /* on, fault on violation, last span infinite */
    for (int i = 0; i < 31; i++) adreno_write(CP_PROTECT(i), protect[i]);
    adreno_write(CP_PROTECT(31), protect[31]);
    adreno_write(RBBM_INT_0_MASK, 0);                                  /* polled, for now */

    /* what Mesa's device table says an A610 wants (a6xx_gen1_low magic regs) */
    adreno_write(RB_DBG_ECO_CNTL, 0x04100000);
    adreno_write(RB_RBP_CNTL, 1);
    static const uint32_t magic[][2] = { { 0x9804, 0xf }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0 }, { 0x8600, 0 },
        { 0xae03, 0 }, { 0xae00, 0 }, { 0xbe04, 0 }, { 0x9600, 0 }, { 0x0e12, 0x10000000 } };
    for (size_t i = 0; i < sizeof magic / sizeof magic[0]; i++) adreno_write(magic[i][0], magic[i][1]);
    /* the CCU (the render backend's cache) in system-memory mode: depth at 0, colour after its 8 KiB */
    adreno_write(RB_CCU_CNTL, (0x2000 >> 12) << 23);
}

/* ---- the zap shader, through TrustZone ------------------------------------------------- */


static const uint8_t *zap_file(const char *path, size_t *len)
{
    struct vnode *v = vfs_lookup(vfs_root(), path);
    return v ? vfs_read_all(v, len) : NULL;
}

/* Words to memory mapped as device memory (no unaligned or partial stores there). */
static void copy_words(volatile uint32_t *dst, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        memcpy(&w, src + i, len - i < 4 ? len - i : 4);
        dst[i / 4] = w;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* TrustZone locks the buffers of a PAS call with the XPU while it works on
 * them, and any CPU access then, even a speculative prefetch through a
 * cacheable mapping, makes it panic: Qualcomm's own driver asks for
 * "non-cachable" memory. So everything goes into the zap shader's reserved
 * region, which sic's direct map leaves out, mapped as device memory (never
 * speculated into): the segment at its start. The metadata (6.7 KiB) does
 * not fit beside it, so it goes to the start of the video firmware's
 * reserved region (also no-map; sic never loads that firmware), kept until
 * the image is authenticated. */
static int zap_load(void)
{
    size_t mlen, slen;
    const uint8_t *mdt = zap_file("/lib/firmware/a610_zap.mdt", &mlen), *seg = zap_file("/lib/firmware/a610_zap.b02", &slen);
    if (!mdt || !seg || mlen < 0x34) { kprintf("adreno: zap: no a610_zap.mdt / .b02\n"); return -1; }
    int n = fdt_find_compatible("qcom,kgsl-3d0");
    int zn = n >= 0 ? fdt_first_child(n) : -1;
    while (zn >= 0 && strcmp(fdt_name(zn), "zap-shader") != 0) zn = fdt_next_sibling(zn);
    uint32_t ph = zn >= 0 ? fdt_prop_u32(zn, "memory-region", 0, 0) : 0;
    uint64_t mem = 0, memsz = 0;
    for (int m = fdt_path("/"); ph && m >= 0; m = fdt_walk_next(m))
        if (fdt_prop_u32(m, "phandle", 0, 0) == ph) { fdt_reg(m, 0, &mem, &memsz); break; }
    if (!mem || (mem & (PAGE_SIZE - 1)) || slen > memsz) {
        kprintf("adreno: zap: the reserved region (%lx+%lx) does not fit it\n", (unsigned long)mem, (unsigned long)memsz);
        return -1;
    }
    uint64_t meta = 0, metasz = 0;
    for (int m = fdt_path("/"); m >= 0; m = fdt_walk_next(m))
        if (memcmp(fdt_name(m), "video_region@", 13) == 0 && fdt_prop(m, "no-map", NULL)) { fdt_reg(m, 0, &meta, &metasz); break; }
    if (!meta || (meta & (PAGE_SIZE - 1)) || mlen > metasz) { kprintf("adreno: zap: no no-map region for the metadata\n"); return -1; }
    volatile uint32_t *region = vmm_map_mmio(mem, memsz), *mregion = vmm_map_mmio(meta, PAGE_ALIGN_UP(mlen));
    if (!region || !mregion) return -1;
    copy_words(mregion, mdt, mlen);
    copy_words(region, seg, slen);

    kprintf("adreno: zap: init image (metadata at %lx)\n", (unsigned long)meta);
    struct scm_res r = qcom_scm_call(SCM_PIL, 1, 2 | 2u << 6, GPU_PAS_ID, meta, 0);       /* INIT_IMAGE (val, rw) */
    kprintf("adreno: zap: init image: status %ld %lu\n", (long)r.a0, (unsigned long)r.a1);
    if (r.a0 || r.a1) return -1;
    r = qcom_scm_call(SCM_PIL, 2, 3, GPU_PAS_ID, mem, PAGE_ALIGN_UP(slen));              /* MEM_SETUP: whole pages, as Linux's mdt loader */
    kprintf("adreno: zap: mem setup %lx+%lx: status %ld %lu\n", (unsigned long)mem, (unsigned long)PAGE_ALIGN_UP(slen), (long)r.a0, (unsigned long)r.a1);
    if (r.a0 || r.a1) return -1;
    r = qcom_scm_call(SCM_PIL, 5, 1, GPU_PAS_ID, 0, 0);                                  /* AUTH_AND_RESET */
    kprintf("adreno: zap: auth and reset: status %ld %lu\n", (long)r.a0, (unsigned long)r.a1);
    return r.a0 || r.a1 ? -1 : 0;
}

int adreno_start(void)
{
    if (started) return 0;
    if (adreno_power_on() != 0) return -1;
    if (gpu_vm_init() != 0) return -1;

    size_t flen;
    const uint8_t *fw = zap_file("/lib/firmware/a630_sqe.fw", &flen);
    if (!fw || flen < 8) { kprintf("adreno: no /lib/firmware/a630_sqe.fw\n"); return -1; }
    kprintf("adreno: SQE microcode version %x\n", (fw[4] | fw[5] << 8) & 0xfff);
    if (platform.fb_base && platform.fb_base + (uint64_t)platform.fb_pitch * platform.fb_height <= 0x100000000ULL)
        gpu_map(platform.fb_base, (uint64_t)platform.fb_pitch * platform.fb_height);   /* the screen, for the 2D engine */
    uint32_t *sqe = gpu_alloc(flen - 4, &sqe_phys);
    ring = gpu_alloc(RING_BYTES, &ring_phys);
    memstore = gpu_alloc(PAGE_SIZE, &memstore_phys);
    if (!sqe || !ring || !memstore) return -1;
    memcpy(sqe, fw + 4, flen - 4);                                         /* the first word is not the CP's */

    kprintf("adreno: registers, microcode, ring\n");
    hw_init();
    adreno_write64(CP_SQE_INSTR_BASE, sqe_phys);
    adreno_write64(CP_RB_BASE, ring_phys);
    adreno_write(CP_RB_CNTL, 12 | 2u << 8 | 1u << 27);                     /* 32 KiB, 32-byte blocks, no rptr write-back */
    wptr = 0;
    adreno_write(CP_SQE_CNTL, 1);

    out_pkt7(CP_ME_INIT, 8);
    out(0x2f); out(3); out(0x20000000); out(0); out(0); out(0); out(0); out(0);
    flush();
    if (idle("CP_ME_INIT") != 0) return -1;
    kprintf("adreno: the CP runs its microcode (CP_ME_INIT done)\n");

    /* Out of secure mode: TrustZone loads the zap shader, then the CP can
     * switch. In secure mode the CP's own writes land but the render
     * backend's do not. (Never RBBM_SECVID_TRUST_CNTL from here: that
     * register is TrustZone's, and touching it froze the chip.) */
    if (zap_load() != 0) { kprintf("adreno: no zap shader: the GPU stays in secure mode and cannot draw\n"); return -1; }
    out_pkt7(CP_SET_SECURE_MODE, 1); out(0);
    flush();
    if (idle("CP_SET_SECURE_MODE") != 0) return -1;
    kprintf("adreno: out of secure mode\n");
    out_pkt7(CP_SET_PROTECTED_MODE, 1); out(1);                         /* from here on, CP_PROTECT holds */

    /* the first fence: the CP writes a word to memory, the CPU reads it */
    out_pkt7(CP_MEM_WRITE, 3);
    out((uint32_t)memstore_phys); out((uint32_t)(memstore_phys >> 32)); out(0x5ee1600d);
    flush();
    if (idle("CP_MEM_WRITE") != 0) return -1;
    for (int i = 0; i < 100 && memstore[0] != 0x5ee1600d; i++) task_sleep_ms(1);
    kprintf("adreno: fence: memory has %08x (%s)\n", memstore[0], memstore[0] == 0x5ee1600d ? "the GPU wrote it" : "nothing came");
    started = memstore[0] == 0x5ee1600d;
    return started ? 0 : -1;
}

/* A solid rectangle in the frame buffer, drawn by the GPU's 2D engine
 * (turnip's r2d clear), then the CCU flushed and a fence written. The 2D
 * engine wants a 64-byte-aligned base and pitch; the splash's pitch is 4320
 * (1080 * 4). Twice that is aligned: the even rows are one surface at the
 * base, the odd ones another 4288 bytes in, eight pixels to the right. */
static uint32_t fence_seq;

static void blit_solid(uint64_t base, uint32_t pitch, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    uint32_t cntl = 1u << 7 | FMT6_8_8_8_8_UNORM << 8 | 0xfu << 20;   /* solid colour, RGBA8, all channels, ifmt UNORM8 */
    out_reg(RB_A2D_PIXEL_CNTL, 0);
    out_reg(RB_A2D_BLT_CNTL, cntl);
    out_reg(GRAS_A2D_BLT_CNTL, cntl);
    out_reg(SP_A2D_OUTPUT_INFO, FMT6_8_8_8_8_UNORM << 3 | 0xfu << 12);
    out_pkt4(RB_A2D_CLEAR_COLOR_DW0, 4);
    out(rgb >> 16 & 0xff); out(rgb >> 8 & 0xff); out(rgb & 0xff); out(0xff);
    out_pkt4(RB_A2D_DEST_BUFFER_INFO, 4);
    out(FMT6_8_8_8_8_UNORM | SWAP_WXYZ << 10); out((uint32_t)base); out((uint32_t)(base >> 32)); out(pitch >> 6);
    out_pkt4(GRAS_A2D_DEST_TL, 2);
    out(x | y << 16); out((x + w - 1) | (y + h - 1) << 16);
    out_pkt7(CP_BLIT, 1); out(3);                                       /* BLIT_OP_SCALE */
}

int adreno_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!started || !w || !h || platform.fb_bpp != 32) return -1;
    uint64_t fb = platform.fb_base;
    uint32_t pitch = platform.fb_pitch;
    if (fb & 63) return -1;
    out_pkt7(CP_WAIT_FOR_IDLE, 0);
    if (!(pitch & 63)) {
        blit_solid(fb, pitch, x, y, w, h, rgb);
    } else if (!((2 * pitch) & 63)) {
        uint32_t shift = (pitch & 63) / 4;                              /* pixels the odd rows' surface starts early */
        uint32_t even0 = (y + 1) / 2, even1 = (y + h + 1) / 2;          /* rows 2i in [y, y+h) */
        uint32_t odd0 = y / 2, odd1 = (y + h) / 2;                      /* rows 2i+1 in [y, y+h) */
        if (even1 > even0) blit_solid(fb, 2 * pitch, x, even0, w, even1 - even0, rgb);
        if (odd1 > odd0) blit_solid(fb + (pitch & ~63u), 2 * pitch, x + shift, odd0, w, odd1 - odd0, rgb);
    } else {
        return -1;
    }
    out_pkt7(CP_EVENT_WRITE, 4); out(PC_CCU_FLUSH_COLOR_TS); out((uint32_t)memstore_phys + 4); out((uint32_t)(memstore_phys >> 32)); out(++fence_seq);
    out_pkt7(CP_EVENT_WRITE, 4); out(CACHE_FLUSH_TS); out((uint32_t)memstore_phys + 8); out((uint32_t)(memstore_phys >> 32)); out(fence_seq);
    flush();
    if (idle("fill") != 0) return -1;
    for (int i = 0; i < 100 && memstore[2] != fence_seq; i++) task_sleep_ms(1);
    kprintf("adreno: fill %ux%u at %u,%u colour %06x: fence %u/%u\n", w, h, x, y, rgb, memstore[2], fence_seq);
    return memstore[2] == fence_seq ? 0 : -1;
}

static void draw_test(void)
{
    /* first off screen, where nothing else draws: a 64x16 surface, pitch 256 */
    uint64_t tphys;
    volatile uint32_t *t = gpu_alloc(PAGE_SIZE, &tphys);
    if (t) {
        out_pkt7(CP_WAIT_FOR_IDLE, 0);
        blit_solid(tphys, 256, 8, 4, 16, 8, 0x336699);
        out_pkt7(CP_EVENT_WRITE, 4); out(PC_CCU_FLUSH_COLOR_TS); out((uint32_t)memstore_phys + 4); out((uint32_t)(memstore_phys >> 32)); out(++fence_seq);
        out_pkt7(CP_EVENT_WRITE, 4); out(CACHE_FLUSH_TS); out((uint32_t)memstore_phys + 8); out((uint32_t)(memstore_phys >> 32)); out(fence_seq);
        flush();
        idle("test fill");
        task_sleep_ms(5);
        kprintf("adreno: test surface: (0,0) %08x (8,4) %08x (23,11) %08x (24,12) %08x\n", t[0], t[4 * 64 + 8], t[11 * 64 + 23], t[12 * 64 + 24]);
    }
    /* proof on the screen: a green and a red bar, drawn by the GPU */
    adreno_fill(100, 700, platform.fb_width - 200, 250, 0x00ff00);
    adreno_fill(100, 1000, platform.fb_width - 200, 250, 0xff0000);
}

static void start_thread(void *arg)
{
    (void)arg;
    if (adreno_start() != 0) { kprintf("adreno: start failed\n"); return; }
    kprintf("adreno: started\n");
    draw_test();
}

/* ---- /dev/adrenogpu: buffers and submissions for programs ----------------------------- */

#define MAX_BOS     512
#define IB_SLOTS    8
#define IB_DWORDS   (64u * 1024)

struct gbo {
    uint32_t handle, size;
    uint64_t phys;                          /* also its GPU address */
    int refs;                               /* the files that own or opened it */
    uint32_t last_fence;                    /* freeing waits for this */
};

struct gpu_file {
    uint32_t handles[256];
    int nhandles;
};

static struct gbo bos[MAX_BOS];
static uint32_t next_handle = 1;
static volatile int busy;                   /* the ring (a sleeping lock) */
static uint32_t *ib[IB_SLOTS];              /* kernel copies of submissions, GPU-mapped */
static uint64_t ib_phys[IB_SLOTS];
static uint32_t ib_fence[IB_SLOTS], ib_next;

static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) task_sleep_ms(1); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }

static uint32_t fence_done(void) { return memstore ? memstore[2] : 0; }

static int fence_wait(uint32_t f, uint32_t timeout_ms)
{
    uint64_t t0 = timer_ms();
    int spins = 0;
    while ((int32_t)(fence_done() - f) < 0) {
        if (timer_ms() - t0 > timeout_ms) {
            kprintf("adreno: fence %u timed out (done %u, rptr %u wptr %u, RBBM_STATUS %08x, CP hw fault %08x, SMMU FSR %08x)\n", f, fence_done(),
                    adreno_read(CP_RB_RPTR), wptr, adreno_read(RBBM_STATUS), adreno_read(CP_HW_FAULT), rd(SMMU_CB(0), CB_FSR));
            return -ETIMEDOUT;
        }
        if (++spins > 200) task_sleep_ms(1);
    }
    return 0;
}

static uint32_t emit_fence(void)
{
    ++fence_seq;
    out_pkt7(CP_EVENT_WRITE, 4); out(PC_CCU_FLUSH_COLOR_TS); out((uint32_t)memstore_phys + 4); out((uint32_t)(memstore_phys >> 32)); out(fence_seq);
    out_pkt7(CP_EVENT_WRITE, 4); out(CACHE_FLUSH_TS); out((uint32_t)memstore_phys + 8); out((uint32_t)(memstore_phys >> 32)); out(fence_seq);
    flush();
    return fence_seq;
}

static struct gbo *bo_get(uint32_t handle)
{
    for (int i = 0; i < MAX_BOS; i++) if (handle && bos[i].handle == handle) return &bos[i];
    return NULL;
}

static int file_has(struct gpu_file *f, uint32_t handle)
{
    for (int i = 0; i < f->nhandles; i++) if (f->handles[i] == handle) return 1;
    return 0;
}

static void bo_unref(struct gbo *b)
{
    if (--b->refs > 0) return;
    fence_wait(b->last_fence, 2000);        /* the GPU may still use it */
    gpu_unmap(b->phys, b->size);
    pmm_free_pages(b->phys, b->size / PAGE_SIZE);
    memset(b, 0, sizeof *b);
}

static void file_drop(struct gpu_file *f, uint32_t handle)
{
    for (int i = 0; i < f->nhandles; i++)
        if (f->handles[i] == handle) {
            f->handles[i] = f->handles[--f->nhandles];
            struct gbo *b = bo_get(handle);
            if (b) bo_unref(b);
            return;
        }
}

static long gpu_open(struct file *fl)
{
    if (!started && adreno_start() != 0) return -EIO;
    if (!ib[0])
        for (int i = 0; i < IB_SLOTS; i++)
            if (!(ib[i] = gpu_alloc(IB_DWORDS * 4, &ib_phys[i]))) return -ENOMEM;
    struct gpu_file *f = kzalloc(sizeof *f);
    if (!f) return -ENOMEM;
    fl->priv_gpu = f;
    return 0;
}

static void gpu_release(struct file *fl)
{
    struct gpu_file *f = fl->priv_gpu;
    if (!f) return;
    lock();
    while (f->nhandles) file_drop(f, f->handles[0]);
    unlock();
    kfree(f);
    fl->priv_gpu = NULL;
}

static void bo_out(struct adreno_bo_info *u, struct gbo *b) { u->handle = b->handle; u->size = b->size; u->gpuaddr = b->phys; u->offset = b->phys; }

static long gpu_ioctl(struct file *fl, long req, uint64_t arg)
{
    struct gpu_file *f = fl->priv_gpu;
    if (!f) return -EINVAL;
    switch (req) {
    case ADRENO_IOC_INFO: {
        if (!user_ok(arg, sizeof(struct adreno_info))) return -EFAULT;
        struct adreno_info in = { 0x06010001, 0x21000, platform.fb_width, platform.fb_height, platform.fb_base, platform.fb_pitch, fence_done() };
        memcpy((void *)arg, &in, sizeof in);
        return 0;
    }
    case ADRENO_IOC_BO_NEW: {
        if (!user_ok(arg, sizeof(struct adreno_bo_info))) return -EFAULT;
        struct adreno_bo_info *u = (void *)arg;
        uint32_t size = (u->size + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        if (!size || f->nhandles >= 256) return -EINVAL;
        lock();
        struct gbo *b = NULL;
        for (int i = 0; i < MAX_BOS && !b; i++) if (!bos[i].handle) b = &bos[i];
        uint64_t phys;
        if (!b || !gpu_alloc(size, &phys)) { unlock(); return -ENOMEM; }
        b->handle = next_handle++;
        if (!next_handle) next_handle = 1;
        b->size = size; b->phys = phys; b->refs = 1; b->last_fence = fence_seq;
        f->handles[f->nhandles++] = b->handle;
        bo_out(u, b);
        unlock();
        return 0;
    }
    case ADRENO_IOC_BO_FREE:
        lock();
        if (file_has(f, (uint32_t)arg)) file_drop(f, (uint32_t)arg);
        unlock();
        return 0;
    case ADRENO_IOC_BO_OPEN: {
        if (!user_ok(arg, sizeof(struct adreno_bo_info))) return -EFAULT;
        struct adreno_bo_info *u = (void *)arg;
        lock();
        struct gbo *b = bo_get(u->handle);
        if (!b || f->nhandles >= 256) { unlock(); return -ENOENT; }
        if (!file_has(f, b->handle)) { b->refs++; f->handles[f->nhandles++] = b->handle; }
        bo_out(u, b);
        unlock();
        return 0;
    }
    case ADRENO_IOC_SUBMIT: {
        if (!user_ok(arg, sizeof(struct adreno_submit))) return -EFAULT;
        struct adreno_submit *u = (void *)arg;
        uint32_t n = u->dwords;
        if (!n || n > IB_DWORDS || !user_ok(u->cmds, (uint64_t)n * 4)) return -EINVAL;
        lock();
        uint32_t slot = ib_next++ % IB_SLOTS;
        if (fence_wait(ib_fence[slot], 2000) != 0) { unlock(); return -EIO; }   /* the slot's last run */
        memcpy(ib[slot], (const void *)u->cmds, (size_t)n * 4);                 /* run from our copy */
        out_pkt7(CP_INDIRECT_BUFFER, 3); out((uint32_t)ib_phys[slot]); out((uint32_t)(ib_phys[slot] >> 32)); out(n);
        uint32_t fence = emit_fence();
        ib_fence[slot] = fence;
        for (int i = 0; i < f->nhandles; i++) { struct gbo *b = bo_get(f->handles[i]); if (b) b->last_fence = fence; }
        u->fence = fence;
        unlock();
        return 0;
    }
    case ADRENO_IOC_WAIT: {
        if (!user_ok(arg, sizeof(struct adreno_wait))) return -EFAULT;
        struct adreno_wait *u = (void *)arg;
        return fence_wait(u->fence, u->timeout_ms ? u->timeout_ms : 2000);
    }
    }
    return -EINVAL;
}

/* Buffers only (and the screen, which the 2D engine draws to anyway), uncached. */
static int gpu_mmap(struct file *fl, uint64_t virt, size_t pages, uint64_t off, int prot)
{
    (void)prot;
    struct gpu_file *f = fl->priv_gpu;
    if (!f) return -EINVAL;
    lock();
    int ok = 0;
    for (int i = 0; i < f->nhandles && !ok; i++) {
        struct gbo *b = bo_get(f->handles[i]);
        if (b && off >= b->phys && off + pages * PAGE_SIZE <= b->phys + b->size) ok = 1;
    }
    unlock();
    if (!ok) return -EINVAL;
    struct task *t = task_current();
    for (size_t i = 0; i < pages; i++)
        if (vmm_map_user_page(t->mm->pgd, virt + i * PAGE_SIZE, off + i * PAGE_SIZE, PTE_WRITE | PTE_DEV | PTE_WC) != 0)
            return -ENOMEM;
    return 0;
}

static const struct dev_ops gpu_ops = { .open = gpu_open, .ioctl = gpu_ioctl, .mmap = gpu_mmap, .release = gpu_release };

/* ---- /dev/adreno ------------------------------------------------------------------- */

static char report[2048];

static size_t make_report(void)
{
    size_t n = 0;
#define P(...) n += (size_t)ksnprintf(report + n, sizeof report - n, __VA_ARGS__)
    P("power: CX %s, GX %s, core clock %s\n",
      rd(cc, CC_CX_GDSCR) & GDSC_PWR_ON ? "on" : "off", rd(cc, CC_GX_GDSCR) & GDSC_PWR_ON ? "on" : "off",
      rd(cc, CC_GX_GFX3D_CBCR) & CBCR_OFF ? "off" : "running");
    if (!powered) { P("(write \"on\" to power it, \"start\" to start it)\n"); return n; }
    P("RBBM_STATUS %08x  CP: rptr %u wptr %u, hw fault %08x, int %08x\n", adreno_read(RBBM_STATUS),
      adreno_read(CP_RB_RPTR), adreno_read(CP_RB_WPTR), adreno_read(CP_HW_FAULT), adreno_read(RBBM_INT_0_STATUS));
    /* the GPU's SMMU (arm,mmu-500 style): global space, then the context banks */
    uint32_t idr1 = rd(smmu, 0x24), page = idr1 >> 31 ? 0x10000 : 0x1000;
    uint32_t numpage = 1u << (((idr1 >> 28) & 7) + 1);
    P("smmu: sCR0 %08x IDR0 %08x IDR1 %08x GFSR %08x GFSYNR0 %08x GFAR %08x\n", rd(smmu, 0), rd(smmu, 0x20), idr1,
      rd(smmu, 0x48), rd(smmu, 0x50), rd(smmu, 0x40));
    for (int i = 0; i < 4; i++) P("  SMR%d %08x S2CR%d %08x\n", i, rd(smmu, 0x800 + 4 * i), i, rd(smmu, 0xc00 + 4 * i));
    for (int i = 0; i < 5; i++) {
        volatile uint8_t *cb = smmu + (numpage + (uint32_t)i) * page;
        if ((numpage + (uint32_t)i + 1) * page > 0x10000) break;
        P("  CB%d: CBAR %08x SCTLR %08x TTBR0 %08x%08x TCR %08x FSR %08x FAR %08x%08x\n", i, rd(smmu, 0x1000 + 4 * i),
          rd(cb, 0), rd(cb, 0x24), rd(cb, 0x20), rd(cb, 0x30), rd(cb, 0x58), rd(cb, 0x64), rd(cb, 0x60));
    }
#undef P
    return n;
}

void qcom_gpu_dump(void)
{
    if (!gpu) return;
    make_report();
    kputs(report);
}

static long adreno_dev_read(struct file *f, void *buf, size_t len)
{
    size_t n = make_report();
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static long adreno_dev_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    if (len >= 2 && memcmp(buf, "on", 2) == 0) return adreno_power_on() == 0 ? (long)len : -EIO;
    if (len >= 5 && memcmp(buf, "start", 5) == 0) {             /* on its own thread: a hang there leaves the shell */
        static int asked;
        if (!asked++) task_create("adreno", start_thread, NULL);
        return (long)len;
    }
    return -EINVAL;
}

static const struct dev_ops adreno_ops = { .read = adreno_dev_read, .write = adreno_dev_write };

void qcom_gpu_init(void)
{
    int n = fdt_find_compatible("qcom,kgsl-3d0");
    uint64_t base;
    if (n < 0 || fdt_reg(n, 0, &base, NULL) != 0 || base != GPU) return;
    gcc = P2V(GCC); cc = P2V(GPUCC); gpu = P2V(GPU); smmu = P2V(GPU_SMMU);
    vfs_mkdev("/dev/adreno", &adreno_ops, NULL);
    vfs_mkdev("/dev/adrenogpu", &gpu_ops, NULL);
    kprintf("adreno: A610 (chip id %x) at %x: /dev/adreno\n", fdt_prop_u32(n, "qcom,chipid", 0, 0), GPU);
}
