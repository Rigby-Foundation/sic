/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PCI enumeration. The configuration-space mechanism itself is the
 * architecture's (asm/pci.h): 0xCF8/0xCFC ports on x86, the host bridge's
 * registers on a PowerMac. */
#include "drivers/pci.h"
#include "asm/pci.h"
#include "printf.h"

static struct pci_dev devs[PCI_MAX_DEVS];
static size_t ndevs;

/* Where no firmware ran before us (aarch64 virt) the BARs are empty and
 * the windows are ours to hand out: sized by writing all-ones, placed
 * naturally aligned, bumping upwards. */
static uint64_t win_mem, win_mem_end, win_io, win_io_end;
static int assign_bars;

static uint64_t take(uint64_t *next, uint64_t end, uint64_t size)
{
    uint64_t base = (*next + size - 1) & ~(size - 1);
    if (base + size > end) return 0;
    *next = base + size;
    return base;
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return arch_pci_read32(bus, slot, func, off);
}

int pci_irq(const struct pci_dev *d)
{
    return arch_pci_irq(d->bus, d->slot, d->func);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    arch_pci_write32(bus, slot, func, off, v);
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
    if (assign_bars && hdr == 0) {
        uint32_t cmd = pci_read32(bus, slot, func, 4);
        pci_write32(bus, slot, func, 4, cmd & ~3u);            /* decoding off while the BARs move */
        for (int i = 0; i < nbars; i++) {
            uint8_t off = (uint8_t)(0x10 + i * 4);
            uint32_t old = pci_read32(bus, slot, func, off);
            pci_write32(bus, slot, func, off, 0xFFFFFFFF);
            uint32_t sz = pci_read32(bus, slot, func, off);
            pci_write32(bus, slot, func, off, old);
            if (sz == 0 || sz == 0xFFFFFFFF) continue;
            if (old & 1) {                                     /* I/O */
                uint32_t size = ~(sz & ~3u) + 1;
                uint64_t base = take(&win_io, win_io_end, size ? size : 4);
                pci_write32(bus, slot, func, off, (uint32_t)base | 1);
            } else if (((old >> 1) & 3) == 2 && i + 1 < nbars) {   /* 64-bit */
                uint8_t off2 = (uint8_t)(off + 4);
                uint32_t old2 = pci_read32(bus, slot, func, off2);
                pci_write32(bus, slot, func, off2, 0xFFFFFFFF);
                uint32_t sz2 = pci_read32(bus, slot, func, off2);
                pci_write32(bus, slot, func, off2, old2);
                uint64_t size = ~(((uint64_t)sz2 << 32) | (sz & ~0xFu)) + 1;
                uint64_t base = take(&win_mem, win_mem_end, size);
                pci_write32(bus, slot, func, off, (uint32_t)base | (old & 0xF));
                pci_write32(bus, slot, func, off2, (uint32_t)(base >> 32));
                i++;
            } else {
                uint32_t size = ~(sz & ~0xFu) + 1;
                uint64_t base = take(&win_mem, win_mem_end, size);
                pci_write32(bus, slot, func, off, (uint32_t)base | (old & 0xF));
            }
        }
        pci_write32(bus, slot, func, 4, cmd | 3);               /* I/O and memory decoding */
    }
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
    arch_pci_init();
    uint64_t ms, is;
    if (arch_pci_windows(&win_mem, &ms, &win_io, &is)) {
        win_mem_end = win_mem + ms; win_io_end = win_io + is;
        if (win_io == 0) win_io = 0x1000;                      /* port 0 is not a valid I/O BAR */
        assign_bars = 1;
        kprintf("pci: assigning BARs from %llx (+%llx) and I/O %llx (+%llx)\n", win_mem, ms, win_io, is);
    }
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
        kprintf("  %02x:%02x.%u %04x:%04x class %02x.%02x.%02x bar0 %llx\n",
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
