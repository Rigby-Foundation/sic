/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI configuration space through the Uni-North host bridge (mac99 and the
 * PowerMac G4 family): CONFIG_ADDR at +0x800000, CONFIG_DATA at +0xC00000,
 * little-endian, with Apple's own address format for bus 0: one bit per
 * device number, and the register offset's low three bits taken from the
 * CONFIG_DATA offset rather than CONFIG_ADDR. */
#include "asm/pci.h"
#include "asm/of.h"
#include "mm/vmm.h"
#include "endian.h"
#include "spinlock.h"

static volatile uint8_t *cfg_addr, *cfg_data;
static spinlock_t pci_lock = SPINLOCK_INIT;
volatile uint8_t *ppc_io_base;

void arch_pci_init(void)
{
    if (!platform.pci_cfg_base[0])
        return;
    volatile uint8_t *regs = vmm_map_mmio(platform.pci_cfg_base[0] + 0x800000, 0x1000);
    cfg_addr = regs;
    cfg_data = vmm_map_mmio(platform.pci_cfg_base[0] + 0xC00000, 0x1000);
    if (platform.pci_io_base)
        ppc_io_base = vmm_map_mmio(platform.pci_io_base, 0x10000);
}

int arch_pci_irq(uint8_t bus, uint8_t slot, uint8_t func)
{
    for (int i = 0; i < platform.pci_irq_count; i++)
        if (platform.pci_irq[i].bus == bus && platform.pci_irq[i].slot == slot && platform.pci_irq[i].func == func)
            return platform.pci_irq[i].irq;
    return -1;
}

static uint32_t cfg_encode(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    if (bus == 0)
        return (1u << slot) | ((uint32_t)func << 8) | (off & 0xFC);
    return ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC) | 1;
}

uint32_t arch_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    if (!cfg_addr || slot > 31) return 0xFFFFFFFF;
    unsigned long f = spin_lock_irqsave(&pci_lock);
    mmio_write32(cfg_addr, cfg_encode(bus, slot, func, off));
    __asm__ volatile("sync");
    /* The low register bits come from the data port offset, not CONFIG_ADDR. */
    uint32_t v = mmio_read32(cfg_data + (off & 7));
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

void arch_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    if (!cfg_addr) return;
    unsigned long f = spin_lock_irqsave(&pci_lock);
    mmio_write32(cfg_addr, cfg_encode(bus, slot, func, off));
    __asm__ volatile("sync");
    mmio_write32(cfg_data + (off & 7), v);
    __asm__ volatile("sync");
    spin_unlock_irqrestore(&pci_lock, f);
}
