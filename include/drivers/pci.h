/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if;
    uint64_t bar[6];            /* BAR base addresses (MMIO or I/O); size not tracked */
    uint8_t  bar_is_io[6];
};

#define PCI_MAX_DEVS 32

void     pci_init(void);        /* enumerate bus 0..255 via config ports */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
void     pci_enable_busmaster(const struct pci_dev *d);
size_t   pci_count(void);
const struct pci_dev *pci_get(size_t i);
const struct pci_dev *pci_find_class(uint8_t class, uint8_t subclass, size_t nth);
