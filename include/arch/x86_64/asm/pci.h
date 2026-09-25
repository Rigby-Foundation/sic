/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI configuration space through the legacy 0xCF8/0xCFC ports. */
#pragma once
#include "types.h"
#include "asm/io.h"

static inline uint32_t arch_pci_cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC);
}

static inline uint32_t arch_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(0xCF8, arch_pci_cfg_addr(bus, slot, func, off));
    return inl(0xCFC);
}

static inline void arch_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    outl(0xCF8, arch_pci_cfg_addr(bus, slot, func, off));
    outl(0xCFC, v);
}

static inline void arch_pci_init(void) { }

/* The interrupt line register, as the firmware routed it through the PIC. */
static inline int arch_pci_windows(uint64_t *mb, uint64_t *ms, uint64_t *ib, uint64_t *is) { (void)mb; (void)ms; (void)ib; (void)is; return 0; }   /* the firmware assigned the BARs */
static inline int arch_pci_irq(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint8_t line = arch_pci_read32(bus, slot, func, 0x3C) & 0xFF;
    return line == 0xFF ? -1 : line;
}
