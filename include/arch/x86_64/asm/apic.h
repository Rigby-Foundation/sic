/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define APIC_SPURIOUS_VECTOR 0xFF

int      apic_init(void);                /* LAPIC + IOAPICs from the MADT; 0 on success */
int      apic_enabled(void);
uint32_t lapic_id(void);
void     lapic_eoi(void);

void     lapic_init_ap(void);            /* enable the LAPIC on an AP (BSP did the mapping) */

/* Periodic LAPIC timer delivering `vector` at `hz`; calibrated once on the
 * BSP, then lapic_timer_start() replays the same setup on each AP. */
void     lapic_timer_init(uint8_t vector, uint32_t hz);
void     lapic_timer_start(void);

void     lapic_send_init(uint32_t apic_id);
void     lapic_send_sipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_broadcast_ipi(uint8_t vector);   /* all CPUs except self */

/* Route legacy ISA IRQ `irq` (honouring MADT overrides) to `vector` on this CPU. */
int      ioapic_route_irq(uint8_t irq, uint8_t vector);
int      ioapic_route_irq_flags(uint8_t irq, uint8_t vector, int level_low);
void     ioapic_mask_irq(uint8_t irq);
void     ioapic_unmask_irq(uint8_t irq);
