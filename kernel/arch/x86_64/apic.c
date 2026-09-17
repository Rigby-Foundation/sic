/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "asm/apic.h"
#include "asm/irqflags.h"
#include "asm/acpi.h"
#include "mm/vmm.h"
#include "asm/pit.h"
#include "printf.h"
#include "asm/cpu.h"

/* ---- local APIC ------------------------------------------------------------- */

#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_TPR       0x080
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LVT_MASKED      (1 << 16)
#define TIMER_PERIODIC  (1 << 17)
#define SVR_ENABLE      (1 << 8)

#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define ICR_INIT        0x500
#define ICR_STARTUP     0x600
#define ICR_ASSERT      (1 << 14)
#define ICR_PENDING     (1 << 12)
#define ICR_ALL_BUT_SELF (3 << 18)

#define IA32_APIC_BASE  0x1B

static volatile uint32_t *lapic;
static int enabled;
static uint32_t timer_ticks_per_ms;
static uint32_t timer_count;
static uint8_t  timer_vector;

static inline uint32_t lapic_read(uint32_t reg)           { return lapic[reg / 4]; }
static inline void     lapic_write(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }

uint32_t lapic_id(void)  { return enabled ? lapic_read(LAPIC_ID) >> 24 : 0; }
int      apic_enabled(void) { return enabled; }
void     lapic_eoi(void)    { lapic_write(LAPIC_EOI, 0); }

static void lapic_init(uint64_t phys)
{
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | (1 << 11));      /* global enable */
    lapic = vmm_map_mmio(phys, 0x1000);
    lapic_write(LAPIC_TPR, 0);                                       /* accept everything */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    enabled = 1;
}

void lapic_init_ap(void)
{
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | (1 << 11));
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
}

static void ipi_wait(void)
{
    while (lapic_read(LAPIC_ICR_LO) & ICR_PENDING)
        cpu_relax();
}

static void ipi_send(uint32_t apic_id, uint32_t lo)
{
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, lo);
    ipi_wait();
}

void lapic_send_init(uint32_t apic_id)          { ipi_send(apic_id, ICR_INIT | ICR_ASSERT); }
void lapic_send_sipi(uint32_t apic_id, uint8_t v) { ipi_send(apic_id, ICR_STARTUP | v); }
void lapic_send_ipi(uint32_t apic_id, uint8_t v)  { ipi_send(apic_id, v | ICR_ASSERT); }
void lapic_broadcast_ipi(uint8_t v)             { ipi_send(0, v | ICR_ASSERT | ICR_ALL_BUT_SELF); }

void lapic_timer_init(uint8_t vector, uint32_t hz)
{
    lapic_write(LAPIC_TIMER_DIV, 0x3);                               /* divide by 16 */

    /* Calibrate: count down from max for 10 ms of PIT time. */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    pit_wait_ms(10);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);
    timer_ticks_per_ms = elapsed / 10;

    timer_count = timer_ticks_per_ms * 1000 / hz;
    if (timer_count == 0)
        timer_count = 1;
    timer_vector = vector;
    lapic_timer_start();

    kprintf("apic: timer %u ticks/ms after /16, %u Hz -> count %u\n", timer_ticks_per_ms, hz, timer_count);
}

void lapic_timer_start(void)
{
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, timer_vector | TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, timer_count);
}

/* ---- I/O APIC --------------------------------------------------------------- */

#define IOREGSEL 0x00
#define IOWIN    0x10
#define IOAPIC_VER 0x01
#define IOAPIC_REDTBL 0x10

#define RED_MASKED    (1 << 16)
#define RED_LEVEL     (1 << 15)
#define RED_ACTIVE_LOW (1 << 13)

struct ioapic {
    volatile uint32_t *mmio;
    uint32_t gsi_base;
    uint32_t gsi_count;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static uint32_t ioapic_count;

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
    io->mmio[IOREGSEL / 4] = reg;
    return io->mmio[IOWIN / 4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t v)
{
    io->mmio[IOREGSEL / 4] = reg;
    io->mmio[IOWIN / 4] = v;
}

static struct ioapic *ioapic_for_gsi(uint32_t gsi)
{
    for (uint32_t i = 0; i < ioapic_count; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].gsi_count)
            return &ioapics[i];
    return NULL;
}

static void ioapic_init(const struct acpi_madt_info *m)
{
    for (uint32_t i = 0; i < m->ioapic_count; i++) {
        struct ioapic *io = &ioapics[ioapic_count++];
        io->mmio = vmm_map_mmio(m->ioapics[i].addr, 0x20);
        io->gsi_base = m->ioapics[i].gsi_base;
        io->gsi_count = ((ioapic_read(io, IOAPIC_VER) >> 16) & 0xFF) + 1;

        for (uint32_t e = 0; e < io->gsi_count; e++) {      /* mask everything */
            ioapic_write(io, IOAPIC_REDTBL + 2 * e, RED_MASKED);
            ioapic_write(io, IOAPIC_REDTBL + 2 * e + 1, 0);
        }
        kprintf("apic: ioapic %u: gsi %u..%u\n", i, io->gsi_base, io->gsi_base + io->gsi_count - 1);
    }
}

/* Resolve an ISA IRQ to (gsi, redirection flags) using the MADT overrides. */
static uint32_t irq_to_gsi(uint8_t irq, uint32_t *flags)
{
    const struct acpi_madt_info *m = acpi_madt();
    *flags = 0;                                 /* ISA default: edge, active high */
    for (uint32_t i = 0; m && i < m->iso_count; i++) {
        const struct acpi_iso *iso = &m->isos[i];
        if (iso->source != irq)
            continue;
        if ((iso->flags & 3) == 3)      *flags |= RED_ACTIVE_LOW;
        if (((iso->flags >> 2) & 3) == 3) *flags |= RED_LEVEL;
        return iso->gsi;
    }
    return irq;
}

int ioapic_route_irq_flags(uint8_t irq, uint8_t vector, int level_low)
{
    uint32_t flags;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    if (level_low)
        flags = RED_LEVEL | RED_ACTIVE_LOW;
    struct ioapic *io = ioapic_for_gsi(gsi);
    if (!io)
        return -1;
    uint32_t e = gsi - io->gsi_base;
    ioapic_write(io, IOAPIC_REDTBL + 2 * e + 1, lapic_id() << 24);          /* physical dest */
    ioapic_write(io, IOAPIC_REDTBL + 2 * e, vector | flags | RED_MASKED);   /* fixed, unmasked later */
    return 0;
}

int ioapic_route_irq(uint8_t irq, uint8_t vector)
{
    return ioapic_route_irq_flags(irq, vector, 0);
}

static void ioapic_set_mask(uint8_t irq, int masked)
{
    uint32_t flags;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    struct ioapic *io = ioapic_for_gsi(gsi);
    if (!io)
        return;
    uint32_t reg = IOAPIC_REDTBL + 2 * (gsi - io->gsi_base);
    uint32_t v = ioapic_read(io, reg);
    ioapic_write(io, reg, masked ? v | RED_MASKED : v & ~RED_MASKED);
}

void ioapic_mask_irq(uint8_t irq)   { ioapic_set_mask(irq, 1); }
void ioapic_unmask_irq(uint8_t irq) { ioapic_set_mask(irq, 0); }

int apic_init(void)
{
    const struct acpi_madt_info *m = acpi_madt();
    if (!m || m->ioapic_count == 0) {
        kprintf("apic: no MADT/IOAPIC, staying on the PIC\n");
        return -1;
    }
    lapic_init(m->lapic_addr);
    ioapic_init(m);
    kprintf("apic: lapic id %u enabled\n", lapic_id());
    return 0;
}
