/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ACPI: RSDP -> RSDT/XSDT -> tables. We only interpret the MADT for now. */
#include "asm/acpi.h"
#include "mm/vmm.h"
#include "printf.h"
#include "string.h"

struct rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_ISO            2
#define MADT_LAPIC_OVERRIDE 5

static const struct acpi_sdt_header *root;    /* RSDT or XSDT, via HHDM */
static int   root_is_xsdt;
static struct acpi_madt_info madt_info;
static int   have_madt;

static int checksum_ok(const void *p, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += ((const uint8_t *)p)[i];
    return sum == 0;
}

static size_t root_entries(void)
{
    return (root->length - sizeof(*root)) / (root_is_xsdt ? 8 : 4);
}

static const struct acpi_sdt_header *root_entry(size_t i)
{
    const uint8_t *base = (const uint8_t *)root + sizeof(*root);
    uint64_t phys;
    if (root_is_xsdt)
        memcpy(&phys, base + i * 8, 8);      /* entries are unaligned */
    else {
        uint32_t p32;
        memcpy(&p32, base + i * 4, 4);
        phys = p32;
    }
    return P2V(phys);
}

const struct acpi_sdt_header *acpi_find_table(const char sig[4])
{
    if (!root)
        return NULL;
    for (size_t i = 0; i < root_entries(); i++) {
        const struct acpi_sdt_header *t = root_entry(i);
        if (memcmp(t->signature, sig, 4) == 0 && checksum_ok(t, t->length))
            return t;
    }
    return NULL;
}

static void parse_madt(const struct madt *m)
{
    memset(&madt_info, 0, sizeof(madt_info));
    madt_info.lapic_addr = m->lapic_addr;

    const uint8_t *p = m->entries, *end = (const uint8_t *)m + m->hdr.length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;

        switch (type) {
        case MADT_LAPIC: {
            uint32_t flags;
            memcpy(&flags, p + 4, 4);
            if ((flags & 3) && madt_info.cpu_count < ACPI_MAX_CPUS)   /* enabled or online-capable */
                madt_info.lapic_ids[madt_info.cpu_count++] = p[3];
            break;
        }
        case MADT_IOAPIC:
            if (madt_info.ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &madt_info.ioapics[madt_info.ioapic_count++];
                io->id = p[2];
                memcpy(&io->addr, p + 4, 4);
                memcpy(&io->gsi_base, p + 8, 4);
            }
            break;
        case MADT_ISO:
            if (madt_info.iso_count < ACPI_MAX_ISOS) {
                struct acpi_iso *iso = &madt_info.isos[madt_info.iso_count++];
                iso->bus = p[2];
                iso->source = p[3];
                memcpy(&iso->gsi, p + 4, 4);
                memcpy(&iso->flags, p + 8, 2);
            }
            break;
        case MADT_LAPIC_OVERRIDE:
            memcpy(&madt_info.lapic_addr, p + 4, 8);
            break;
        }
        p += len;
    }
    have_madt = 1;
}

int acpi_init(uint64_t rsdp_phys)
{
    if (!rsdp_phys)
        return -1;
    const struct rsdp *rsdp = P2V(rsdp_phys);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        kprintf("acpi: bad RSDP\n");
        return -1;
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_addr && checksum_ok(rsdp, rsdp->length)) {
        root = P2V(rsdp->xsdt_addr);
        root_is_xsdt = 1;
    } else {
        root = P2V(rsdp->rsdt_addr);
    }
    if (!checksum_ok(root, root->length)) {
        kprintf("acpi: bad %s checksum\n", root_is_xsdt ? "XSDT" : "RSDT");
        root = NULL;
        return -1;
    }

    kprintf("acpi: rev %u, %s with %lu tables:", rsdp->revision,
            root_is_xsdt ? "XSDT" : "RSDT", root_entries());
    for (size_t i = 0; i < root_entries(); i++) {
        const struct acpi_sdt_header *t = root_entry(i);
        kprintf(" %c%c%c%c", t->signature[0], t->signature[1], t->signature[2], t->signature[3]);
    }
    kprintf("\n");

    const struct madt *m = (const struct madt *)acpi_find_table("APIC");
    if (!m) {
        kprintf("acpi: no MADT\n");
        return -1;
    }
    parse_madt(m);

    kprintf("acpi: lapic @ %llx, %u cpu(s), %u ioapic(s), %u override(s)\n",
            madt_info.lapic_addr, madt_info.cpu_count, madt_info.ioapic_count, madt_info.iso_count);
    for (uint32_t i = 0; i < madt_info.ioapic_count; i++)
        kprintf("  ioapic %u @ %x, gsi base %u\n",
                madt_info.ioapics[i].id, madt_info.ioapics[i].addr, madt_info.ioapics[i].gsi_base);
    for (uint32_t i = 0; i < madt_info.iso_count; i++)
        kprintf("  irq %u -> gsi %u (flags %x)\n",
                madt_info.isos[i].source, madt_info.isos[i].gsi, madt_info.isos[i].flags);
    return 0;
}

const struct acpi_madt_info *acpi_madt(void)
{
    return have_madt ? &madt_info : NULL;
}
