/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The OpenPIC (MPIC) in mac-io: interrupt sources for everything on a
 * PowerMac. Registers are little-endian 32-bit words. */
#include "asm/irq.h"
#include "asm/of.h"
#include "endian.h"
#include "proc/sched.h"
#include "proc/signal.h"
#include "printf.h"

#define OPENPIC_FRR       0x01000      /* feature reporting */
#define OPENPIC_GCR       0x01020      /* global config */
#define OPENPIC_SPURIOUS  0x010E0
#define OPENPIC_SRC_VP(n) (0x10000 + (n) * 0x20)   /* vector/priority */
#define OPENPIC_SRC_DST(n) (0x10010 + (n) * 0x20)  /* destination CPUs */
#define OPENPIC_CPU_TASKPRI 0x20080
#define OPENPIC_CPU_IACK    0x200A0
#define OPENPIC_CPU_EOI     0x200B0

#define VP_MASK      0x80000000u
#define VP_ACTIVITY  0x40000000u
#define VP_SENSE     0x00400000u       /* level */
#define VP_POLARITY  0x00800000u       /* active high */

static volatile uint8_t *pic;
#define IRQ_SHARE 4                         /* PCI lines are shared: every handler on a line runs */
static irq_handler_t handlers[IRQ_COUNT][IRQ_SHARE];
static uint32_t nsources;

static inline uint32_t rd(uint32_t off) { return mmio_read32(pic + off); }
static inline void wr(uint32_t off, uint32_t v) { mmio_write32(pic + off, v); }

void ppc_openpic_init(void)
{
    if (!platform.openpic_base) {
        kprintf("openpic: not found in the device tree\n");
        return;
    }
    pic = (volatile uint8_t *)(uintptr_t)platform.openpic_base;     /* BAT-identity mapped */
    uint32_t frr = rd(OPENPIC_FRR);
    nsources = ((frr >> 16) & 0x7FF) + 1;
    if (nsources > IRQ_COUNT) nsources = IRQ_COUNT;
    wr(OPENPIC_GCR, rd(OPENPIC_GCR) | 0x20000000);               /* mixed mode */
    for (uint32_t i = 0; i < nsources; i++) {
        wr(OPENPIC_SRC_VP(i), VP_MASK | (8 << 16) | i);            /* masked, priority 8, vector = source */
        wr(OPENPIC_SRC_DST(i), 1);                                 /* CPU 0 */
    }
    wr(OPENPIC_SPURIOUS, 0xFF);
    wr(OPENPIC_CPU_TASKPRI, 0);                                    /* accept everything */
    kprintf("openpic: %u sources at %x (frr %x)\n", nsources, platform.openpic_base, frr);
}

void irq_install(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT) return;
    for (int i = 0; i < IRQ_SHARE; i++)
        if (!handlers[irq][i] || handlers[irq][i] == handler) { handlers[irq][i] = handler; return; }
}

void irq_mask(uint8_t irq)
{
    if (!pic || irq >= nsources) return;
    wr(OPENPIC_SRC_VP(irq), rd(OPENPIC_SRC_VP(irq)) | VP_MASK);
}

void irq_unmask(uint8_t irq)
{
    if (!pic || irq >= nsources) return;
    wr(OPENPIC_SRC_VP(irq), (rd(OPENPIC_SRC_VP(irq)) & ~VP_MASK));
}

void irq_unmask_pci(uint8_t irq)
{
    if (!pic || irq >= nsources) return;
    /* PCI INTx lines arrive at the Mac's OpenPIC as level, active high
     * (the bridge inverts them; the sense cell in the interrupt-map is 1). */
    uint32_t v = rd(OPENPIC_SRC_VP(irq)) & ~VP_MASK;
    v |= VP_SENSE | VP_POLARITY;
    wr(OPENPIC_SRC_VP(irq), v);
}

void ppc_openpic_sense(uint8_t irq, int level, int active_high)
{
    if (!pic || irq >= nsources) return;
    uint32_t v = rd(OPENPIC_SRC_VP(irq)) & ~(VP_SENSE | VP_POLARITY);
    if (level) v |= VP_SENSE;
    if (active_high) v |= VP_POLARITY;
    wr(OPENPIC_SRC_VP(irq), v);
}

void ppc_openpic_dispatch(struct interrupt_frame *f)
{
    if (!pic) return;
    uint32_t vec = rd(OPENPIC_CPU_IACK) & 0xFF;
    if (vec == 0xFF)
        return;                                                    /* spurious */
    if (vec < IRQ_COUNT && handlers[vec][0])
        for (int i = 0; i < IRQ_SHARE && handlers[vec][i]; i++)
            handlers[vec][i](f);
    else
        kprintf("[unhandled irq %u]\n", vec);
    wr(OPENPIC_CPU_EOI, 0);
    sched_preempt();
}
