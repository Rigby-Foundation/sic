/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI configuration space through the Uni-North host bridge (kernel/arch/powerpc/pci.c). */
#pragma once
#include "types.h"
void     arch_pci_init(void);        /* map the host bridge registers */
uint32_t arch_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     arch_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
int      arch_pci_irq(uint8_t bus, uint8_t slot, uint8_t func);   /* OpenPIC source from the device tree */
static inline int arch_pci_windows(uint64_t *mb, uint64_t *ms, uint64_t *ib, uint64_t *is) { (void)mb; (void)ms; (void)ib; (void)is; return 0; }   /* OpenFirmware assigned the BARs */
