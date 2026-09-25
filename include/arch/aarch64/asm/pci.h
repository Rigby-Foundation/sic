/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI configuration space through ECAM (kernel/arch/aarch64/pci.c). No
 * firmware ran before us, so the generic code assigns the BARs from the
 * windows the device tree describes. */
#pragma once
#include "types.h"
void     arch_pci_init(void);
uint32_t arch_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     arch_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
int      arch_pci_irq(uint8_t bus, uint8_t slot, uint8_t func);   /* GIC interrupt ID from the interrupt map */
/* The address windows for BAR assignment; 0 when the firmware already did it. */
int      arch_pci_windows(uint64_t *mem_base, uint64_t *mem_size, uint64_t *io_base, uint64_t *io_size);
