/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The GICv2 interrupt controller: the distributor routes SPIs to us, the
 * CPU interface hands them over one at a time. Handlers are per interrupt
 * ID, several per line for shared PCI INTx. */
#include "asm/irq.h"
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/sysreg.h"
#include "asm/cpu.h"
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

#define IRQ_SHARE 4
static irq_handler_t handlers[IRQ_COUNT][IRQ_SHARE];
static volatile uint8_t *gicd, *gicc;
static uint32_t nirqs;

static uint32_t d_read(uint32_t off) { return mmio_read32(gicd + off); }
static void d_write(uint32_t off, uint32_t v) { mmio_write32(gicd + off, v); }

#define GICD_SGIR       0xf00
#define IRQ_RESCHED     0           /* the SGI another CPU sends to wake this one */

/* The banked, per-CPU part: SGIs/PPIs and the CPU interface. Every CPU
 * runs it for itself. */
void gic_init_cpu(void)
{
    d_write(GICD_ICENABLER, 0xffffffff);
    d_write(GICD_ICPENDR, 0xffffffff);
    for (uint32_t i = 0; i < 32; i += 4) d_write(GICD_IPRIORITYR + i, 0xa0a0a0a0);
    d_write(GICD_ISENABLER, 1u << IRQ_RESCHED);
    mmio_write32(gicc + GICC_PMR, 0xf0);
    mmio_write32(gicc + GICC_CTLR, 1);
}

static void resched_irq(struct interrupt_frame *f) { (void)f; }     /* the wakeup itself is the point */

void gic_init(void)
{
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

void irq_mask(uint8_t irq)   { if (irq < nirqs) d_write(GICD_ICENABLER + (irq / 32) * 4, 1u << (irq % 32)); }
void irq_unmask(uint8_t irq) { if (irq < nirqs) d_write(GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32)); }
void irq_unmask_pci(uint8_t irq) { irq_unmask(irq); }       /* SPIs are level-triggered already */

/* From the vector: acknowledge, signal the end, run the handlers. The end
 * comes first because a handler may switch tasks (the timer preempting)
 * and not come back for a while; with the interrupt still active the GIC
 * would hold every other one back. Nothing nests: IRQs stay masked here. */
void aarch64_irq(struct interrupt_frame *f)
{
    uint32_t last = ~0u, repeats = 0;
    for (;;) {
        uint32_t iar = mmio_read32(gicc + GICC_IAR), id = iar & 0x3ff;
        if (id >= 1020) break;                  /* spurious: nothing more pending */
        mmio_write32(gicc + GICC_EOIR, iar);
        if (id == last && ++repeats > 64) {     /* a level nobody clears: stop the storm */
            kprintf("gic: interrupt %u stays asserted after its handler, masking it\n", id);
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
