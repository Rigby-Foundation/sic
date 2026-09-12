/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISOS    24
#define ACPI_MAX_CPUS    64

struct acpi_ioapic {
    uint8_t  id;
    uint32_t addr;
    uint32_t gsi_base;
};

struct acpi_iso {             /* interrupt source override: legacy IRQ -> GSI */
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_madt_info {
    uint64_t lapic_addr;
    uint32_t cpu_count;
    uint8_t  lapic_ids[ACPI_MAX_CPUS];
    uint32_t ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    uint32_t iso_count;
    struct acpi_iso isos[ACPI_MAX_ISOS];
};

int  acpi_init(uint64_t rsdp_phys);
const struct acpi_sdt_header *acpi_find_table(const char sig[4]);
const struct acpi_madt_info *acpi_madt(void);
