/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The GIC: the distributor routes SPIs to us, the CPU interface hands
 * them over one at a time. Version 2 (QEMU virt) has a memory-mapped CPU
 * interface; version 3 (phones, newer boards) has a redistributor per CPU
 * for its SGIs and PPIs and system registers for the CPU interface.
 * Handlers are per interrupt ID, several per line for shared PCI INTx. */
#include "asm/irq.h"
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/sysreg.h"
#include "asm/cpu.h"
#include "asm/timer.h"
#include "proc/sched.h"
#include "endian.h"
#include "printf.h"

#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_ICPENDR    0x280
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_ICFGR      0xc00
#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00c
#define GICC_EOIR       0x010

/* GICv3 */
#define GICD_IGROUPR    0x080
#define GICD_IROUTER    0x6000
#define GICD_CTLR_RWP   (1u << 31)
#define GICR_CTLR       0x0000
#define GICR_TYPER      0x0008
#define GICR_WAKER      0x0014
#define GICR_SGI        0x10000     /* the second frame: SGI/PPI registers */
#define GICR_IGROUPR0   0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_ICENABLER0 0x0180
#define GICR_ICPENDR0   0x0280
#define GICR_IPRIORITYR 0x0400
#define ICC_IAR1_EL1    S3_0_C12_C12_0
#define ICC_EOIR1_EL1   S3_0_C12_C12_1
#define ICC_BPR1_EL1    S3_0_C12_C12_3
#define ICC_CTLR_EL1    S3_0_C12_C12_4
#define ICC_SRE_EL1     S3_0_C12_C12_5
#define ICC_IGRPEN1_EL1 S3_0_C12_C12_7
#define ICC_PMR_EL1     S3_0_C4_C6_0
#define ICC_SGI1R_EL1   S3_0_C12_C11_5
#define ICC_RPR_EL1     S3_0_C12_C11_3
#define ICC_HPPIR1_EL1  S3_0_C12_C12_2
#define GICR_ISPENDR0   0x0200
#define GICR_ISACTIVER0 0x0300
#define GICR_ICFGR1     0x0c04
#define SYSREG_R(r)     read_sysreg(r)
#define SYSREG_W(r, v)  write_sysreg(r, v)

#define IRQ_SHARE 4
static irq_handler_t handlers[IRQ_COUNT][IRQ_SHARE];
static volatile uint8_t *gicd, *gicc;
static uint32_t nirqs;
static int v3;
static volatile uint8_t *gicr_sgi[MAX_CPUS];     /* v3: each CPU's SGI/PPI frame */

static uint32_t d_read(uint32_t off) { return mmio_read32(gicd + off); }
static void d_write(uint32_t off, uint32_t v) { mmio_write32(gicd + off, v); }

#define GICD_SGIR       0xf00
#define IRQ_RESCHED     0           /* the SGI another CPU sends to wake this one */

/* MPIDR's affinity as GICR_TYPER and GICD_IROUTER spell it. */
static uint32_t aff_typer(uint64_t mpidr) { return (uint32_t)(mpidr & 0xffffff) | (uint32_t)((mpidr >> 32) & 0xff) << 24; }
static uint64_t aff_route(uint64_t mpidr) { return (mpidr & 0xffffff) | ((mpidr >> 32) & 0xff) << 32; }

/* This CPU's redistributor: the frames in a row, each saying whose it is. */
static volatile uint8_t *v3_find_rd(void)
{
    uint32_t me = aff_typer(read_sysreg(mpidr_el1));
    volatile uint8_t *base = P2V(platform.gicr_base);
    uint64_t span = platform.gicr_size ? platform.gicr_size : 0x100000;
    for (uint64_t off = 0; off < span; ) {
        uint32_t lo = mmio_read32(base + off + GICR_TYPER), hi = mmio_read32(base + off + GICR_TYPER + 4);
        if (hi == me) return base + off;
        if (lo & (1u << 4)) break;                          /* Last */
        off += (lo & (1u << 1)) ? 0x40000 : 0x20000;        /* VLPIS: four frames instead of two */
    }
    return NULL;
}

static void v3_init_cpu(void)
{
    volatile uint8_t *rd = v3_find_rd();
    if (!rd) { kprintf("gic: no redistributor for cpu %u\n", this_cpu()->index); return; }
    /* Wake it (ProcessorSleep off, wait for ChildrenAsleep to clear). Some
     * Qualcomm parts keep GICR_WAKER for the secure side, which has woken
     * the redistributor already; there it never changes: don't wait long. */
    uint32_t w = mmio_read32(rd + GICR_WAKER);
    mmio_write32(rd + GICR_WAKER, w & ~2u);
    for (int i = 0; i < 2000 && (mmio_read32(rd + GICR_WAKER) & 4u); i++) ;
    volatile uint8_t *sgi = rd + GICR_SGI;
    gicr_sgi[this_cpu()->index] = sgi;
    mmio_write32(sgi + GICR_IGROUPR0, 0xffffffff);          /* group 1 (non-secure) */
    mmio_write32(sgi + GICR_ICENABLER0, 0xffffffff);
    mmio_write32(sgi + GICR_ICPENDR0, 0xffffffff);
    for (uint32_t i = 0; i < 32; i += 4) mmio_write32(sgi + GICR_IPRIORITYR + i, 0xa0a0a0a0);
    for (int i = 0; i < 1000000 && (mmio_read32(rd + GICR_CTLR) & (1u << 3)); i++) ;   /* RWP */
    mmio_write32(sgi + GICR_ISENABLER0, 1u << IRQ_RESCHED);

    SYSREG_W(ICC_SRE_EL1, SYSREG_R(ICC_SRE_EL1) | 1);       /* system register interface */
    isb();
    SYSREG_W(ICC_PMR_EL1, 0xf0);
    SYSREG_W(ICC_BPR1_EL1, 0);
    SYSREG_W(ICC_CTLR_EL1, 0);                              /* EOI also deactivates */
    SYSREG_W(ICC_IGRPEN1_EL1, 1);
    isb();
}

static void v3_dist_wait(void)
{
    for (int i = 0; i < 1000000 && (d_read(GICD_CTLR) & GICD_CTLR_RWP); i++) ;
}

/* The banked, per-CPU part: SGIs/PPIs and the CPU interface. Every CPU
 * runs it for itself. */
void gic_init_cpu(void)
{
    if (v3) { v3_init_cpu(); return; }
    d_write(GICD_ICENABLER, 0xffffffff);
    d_write(GICD_ICPENDR, 0xffffffff);
    for (uint32_t i = 0; i < 32; i += 4) d_write(GICD_IPRIORITYR + i, 0xa0a0a0a0);
    d_write(GICD_ISENABLER, 1u << IRQ_RESCHED);
    mmio_write32(gicc + GICC_PMR, 0xf0);
    mmio_write32(gicc + GICC_CTLR, 1);
}

static void resched_irq(struct interrupt_frame *f) { (void)f; }     /* the wakeup itself is the point */

static void gic_init_v3(void)
{
    gicd = P2V(platform.gicd_base);
    v3 = 1;
    nirqs = 32 * ((d_read(GICD_TYPER) & 0x1f) + 1);
    uint32_t ids = nirqs;
    if (nirqs > IRQ_COUNT) nirqs = IRQ_COUNT;
    d_write(GICD_CTLR, 0);
    v3_dist_wait();
    uint64_t route = aff_route(read_sysreg(mpidr_el1));     /* SPIs to the boot CPU */
    for (uint32_t i = 32; i < ids; i += 32) {
        d_write(GICD_IGROUPR + i / 8, 0xffffffff);
        d_write(GICD_ICENABLER + i / 8, 0xffffffff);
        d_write(GICD_ICPENDR + i / 8, 0xffffffff);
    }
    for (uint32_t i = 32; i < ids; i += 4) d_write(GICD_IPRIORITYR + i, 0xa0a0a0a0);
    for (uint32_t i = 32; i < ids; i += 16) d_write(GICD_ICFGR + i / 4, 0);
    for (uint32_t i = 32; i < ids; i++) {
        d_write(GICD_IROUTER + i * 8, (uint32_t)route);
        d_write(GICD_IROUTER + i * 8 + 4, (uint32_t)(route >> 32));
    }
    v3_dist_wait();
    d_write(GICD_CTLR, (1u << 4) | (1u << 1) | 1u);        /* ARE_NS, group 1 enabled */
    v3_dist_wait();
    irq_install(IRQ_RESCHED, resched_irq);
    gic_init_cpu();
    kprintf("gic: v3, %u interrupt ids, distributor %llx, redistributors %llx\n", ids, platform.gicd_base, platform.gicr_base);
}

void gic_init(void)
{
    if (platform.gic_version == 3) { gic_init_v3(); return; }
    gicd = P2V(platform.gicd_base ? platform.gicd_base : 0x08000000);
    gicc = P2V(platform.gicc_base ? platform.gicc_base : 0x08010000);
    nirqs = 32 * ((d_read(GICD_TYPER) & 0x1f) + 1);
    if (nirqs > IRQ_COUNT) nirqs = IRQ_COUNT;
    d_write(GICD_CTLR, 0);
    for (uint32_t i = 32; i < nirqs; i += 32) {
        d_write(GICD_ICENABLER + i / 8, 0xffffffff);
        d_write(GICD_ICPENDR + i / 8, 0xffffffff);
    }
    for (uint32_t i = 32; i < nirqs; i += 4) d_write(GICD_IPRIORITYR + i, 0xa0a0a0a0);
    for (uint32_t i = 32; i < nirqs; i += 4) d_write(GICD_ITARGETSR + i, 0x01010101);   /* SPIs to CPU 0 */
    for (uint32_t i = 32; i < nirqs; i += 16) d_write(GICD_ICFGR + i / 4, 0);            /* level-triggered */
    d_write(GICD_CTLR, 1);
    irq_install(IRQ_RESCHED, resched_irq);
    gic_init_cpu();
    kprintf("gic: v2, %u interrupt ids, distributor %llx\n", nirqs, platform.gicd_base);
}

void arch_wake_cpu(struct cpu *c)
{
    if (v3 && c != this_cpu() && c->online) {
        uint64_t m = c->mpidr;
        dsb(ishst);
        SYSREG_W(ICC_SGI1R_EL1, ((m >> 8) & 0xff) << 16 | (1UL << (m & 0xf)) | (uint64_t)IRQ_RESCHED << 24 |
                                ((m >> 16) & 0xff) << 32 | ((m >> 32) & 0xff) << 48);
        isb();
        return;
    }
    if (c != this_cpu() && c->online) {
        dsb(ishst);
        d_write(GICD_SGIR, (1u << (16 + c->index)) | IRQ_RESCHED);
    }
}

void irq_install(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT) return;
    for (int i = 0; i < IRQ_SHARE; i++)
        if (!handlers[irq][i] || handlers[irq][i] == handler) { handlers[irq][i] = handler; return; }
    kprintf("gic: irq %u: too many handlers\n", irq);
}

void irq_mask(uint8_t irq)
{
    if (v3 && irq < 32) { volatile uint8_t *f = gicr_sgi[this_cpu()->index]; if (f) mmio_write32(f + GICR_ICENABLER0, 1u << irq); return; }
    if (irq < nirqs) d_write(GICD_ICENABLER + (irq / 32) * 4, 1u << (irq % 32));
}

void irq_unmask(uint8_t irq)
{
    if (v3 && irq < 32) { volatile uint8_t *f = gicr_sgi[this_cpu()->index]; if (f) mmio_write32(f + GICR_ISENABLER0, 1u << irq); return; }
    if (irq < nirqs) d_write(GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32));
}
void irq_unmask_pci(uint8_t irq) { irq_unmask(irq); }       /* SPIs are level-triggered already */

/* From the vector: acknowledge, signal the end, run the handlers. The end
 * comes first because a handler may switch tasks (the timer preempting)
 * and not come back for a while; with the interrupt still active the GIC
 * would hold every other one back. Nothing nests: IRQs stay masked here. */
void aarch64_irq(struct interrupt_frame *f)
{
    uint32_t last = ~0u, repeats = 0;
    for (;;) {
        uint32_t iar, id;
        if (v3) {
            iar = (uint32_t)SYSREG_R(ICC_IAR1_EL1); id = iar & 0xffffff;
            if (id >= 1020 && id < 1024) break;
            SYSREG_W(ICC_EOIR1_EL1, iar);
        } else {
            iar = mmio_read32(gicc + GICC_IAR); id = iar & 0x3ff;
            if (id >= 1020) break;              /* spurious: nothing more pending */
            mmio_write32(gicc + GICC_EOIR, iar);
        }
        if (id == last && ++repeats > 64) {     /* a level nobody clears: stop the storm */
            static uint8_t told[MAX_CPUS];
            uint32_t me = this_cpu()->index;
            if (!(told[me] & 1)) {
                told[me] |= 1;
                kprintf("gic: cpu %u (mpidr %llx): interrupt %u stays asserted after its handler, masking it\n",
                        me, read_sysreg(mpidr_el1), id);
                volatile uint8_t *sg = v3 ? gicr_sgi[me] : NULL;
                if (sg)
                    kprintf("gic: cpu %u: iar %x, icc ctlr %llx sre %llx pmr %llx rpr %llx hppir %llx igrpen1 %llx; "
                            "gicr group %x enabled %x pending %x active %x cfg %x waker %x; timer ctl %llx cval %llx cnt %llx\n",
                            me, iar, read_sysreg(ICC_CTLR_EL1), read_sysreg(ICC_SRE_EL1), read_sysreg(ICC_PMR_EL1),
                            read_sysreg(ICC_RPR_EL1), read_sysreg(ICC_HPPIR1_EL1), read_sysreg(ICC_IGRPEN1_EL1),
                            mmio_read32(sg + GICR_IGROUPR0), mmio_read32(sg + GICR_ISENABLER0), mmio_read32(sg + GICR_ISPENDR0),
                            mmio_read32(sg + GICR_ISACTIVER0), mmio_read32(sg + GICR_ICFGR1), mmio_read32(sg - GICR_SGI + GICR_WAKER),
                            read_sysreg(cntv_ctl_el0), read_sysreg(cntv_cval_el0), read_sysreg(cntvct_el0));
                extern struct timer_diag timer_diag[];
                if (id == 27 && timer_diag[me].seen)
                    kprintf("gic: cpu %u timer: counter %llx, deadline %llx, ctl %llx, counter after %llx\n", me,
                            timer_diag[me].cnt, timer_diag[me].cval, timer_diag[me].ctl, timer_diag[me].cnt2);
            }
            irq_mask((uint8_t)id);
            break;
        }
        if (id != last) { last = id; repeats = 0; }
        if (id < IRQ_COUNT) {
            int any = 0;
            for (int i = 0; i < IRQ_SHARE; i++)
                if (handlers[id][i]) { handlers[id][i](f); any = 1; }
            if (!any) { kprintf("gic: unexpected interrupt %u\n", id); irq_mask((uint8_t)id); }
        }
    }
    /* Everything is acknowledged: switch tasks here if a tick or a
     * wakeup asked for it. */
    sched_preempt();
#ifdef CONFIG_SIGNALS
    if (FRAME_FROM_USER(f)) {
        extern void signal_deliver_irq(struct interrupt_frame *f);
        signal_deliver_irq(f);
    }
#endif
}
