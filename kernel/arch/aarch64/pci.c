/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI configuration space through ECAM: bus/device/function/register are
 * bits of the address. The windows and the INTx routing come from the
 * device tree (boot.c). */
#include "asm/pci.h"
#include "asm/fdt.h"
#include "mm/vmm.h"
#include "endian.h"

static volatile uint8_t *ecam;
volatile uint8_t *aarch64_io_base;

void arch_pci_init(void)
{
    if (!platform.ecam_base) return;
    ecam = vmm_map_mmio(platform.ecam_base, platform.ecam_size ? platform.ecam_size : 0x1000000);
    if (platform.pci_io_base)
        aarch64_io_base = vmm_map_mmio(platform.pci_io_base, platform.pci_io_size ? platform.pci_io_size : 0x10000);
}

static volatile uint8_t *cfg(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return ecam + ((uint64_t)bus << 20 | (uint64_t)slot << 15 | (uint64_t)func << 12 | (off & 0xfc));
}

uint32_t arch_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    if (!ecam || slot > 31 || (uint64_t)bus << 20 >= platform.ecam_size) return 0xFFFFFFFF;
    return mmio_read32(cfg(bus, slot, func, off));
}

void arch_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    if (!ecam || slot > 31) return;
    mmio_write32(cfg(bus, slot, func, off), v);
}

int arch_pci_irq(uint8_t bus, uint8_t slot, uint8_t func)
{
    (void)bus;
    uint32_t pin = (arch_pci_read32(bus, slot, func, 0x3c) >> 8) & 0xff;  /* 1 = INTA */
    if (!pin) return -1;
    for (int i = 0; i < platform.pci_irq_count; i++)
        if (platform.pci_irq[i].slot == (slot & platform.pci_irq_slot_mask) && platform.pci_irq[i].pin == pin)
            return platform.pci_irq[i].irq;
    return -1;
}

int arch_pci_windows(uint64_t *mem_base, uint64_t *mem_size, uint64_t *io_base, uint64_t *io_size)
{
    if (!platform.pci_mem_base) return 0;
    *mem_base = platform.pci_mem_base; *mem_size = platform.pci_mem_size;
    *io_base = platform.pci_io_pci_base; *io_size = platform.pci_io_size;
    return 1;
}
