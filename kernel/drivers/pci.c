/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI: configuration space via the legacy 0xCF8/0xCFC ports. */
#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "printf.h"

static struct pci_dev devs[PCI_MAX_DEVS];
static size_t ndevs;

static inline uint32_t addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC);
}

static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(0xCF8, addr(bus, slot, func, off));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    outl(0xCF8, addr(bus, slot, func, off));
    outl(0xCFC, v);
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs >= PCI_MAX_DEVS)
        return;
    struct pci_dev *d = &devs[ndevs++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = pci_read32(bus, slot, func, 8);
    d->class = cls >> 24;
    d->subclass = (cls >> 16) & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;

    uint8_t hdr = (pci_read32(bus, slot, func, 0x0C) >> 16) & 0x7F;
    int nbars = hdr == 0 ? 6 : 2;
    for (int i = 0; i < nbars; i++) {
        uint32_t lo = pci_read32(bus, slot, func, 0x10 + i * 4);
        if (lo & 1) {
            d->bar[i] = lo & ~3u;
            d->bar_is_io[i] = 1;
        } else if (((lo >> 1) & 3) == 2 && i + 1 < nbars) {     /* 64-bit */
            uint32_t hi = pci_read32(bus, slot, func, 0x10 + (i + 1) * 4);
            d->bar[i] = ((uint64_t)hi << 32) | (lo & ~0xFu);
            i++;
        } else {
            d->bar[i] = lo & ~0xFu;
        }
    }
}

void pci_init(void)
{
    for (int bus = 0; bus < 256; bus++)
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            int multi = pci_read32(bus, slot, 0, 0x0C) & 0x800000;
            for (int func = 0; func < (multi ? 8 : 1); func++)
                probe(bus, slot, func);
        }
    kprintf("pci: %lu device(s)\n", ndevs);
    for (size_t i = 0; i < ndevs; i++)
        kprintf("  %02x:%02x.%u %04x:%04x class %02x.%02x.%02x bar0 %lx\n",
                devs[i].bus, devs[i].slot, devs[i].func, devs[i].vendor, devs[i].device,
                devs[i].class, devs[i].subclass, devs[i].prog_if, devs[i].bar[0]);
}

void pci_enable_busmaster(const struct pci_dev *d)
{
    uint32_t cmd = pci_read32(d->bus, d->slot, d->func, 4);
    cmd |= (1 << 1) | (1 << 2);     /* memory space, bus master */
    pci_write32(d->bus, d->slot, d->func, 4, cmd);
}

size_t pci_count(void) { return ndevs; }
const struct pci_dev *pci_get(size_t i) { return i < ndevs ? &devs[i] : NULL; }

const struct pci_dev *pci_find_class(uint8_t class, uint8_t subclass, size_t nth)
{
    for (size_t i = 0; i < ndevs; i++)
        if (devs[i].class == class && devs[i].subclass == subclass && nth-- == 0)
            return &devs[i];
    return NULL;
}
