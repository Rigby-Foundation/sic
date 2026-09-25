/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The flattened device tree the firmware/QEMU hands us in x0, and what the
 * boot code pulled out of it (kernel/arch/aarch64/fdt.c, boot.c). */
#pragma once
#include "types.h"

/* Node offsets are byte offsets into the structure block; -1 = none. */
int  fdt_init(const void *blob);                         /* 0 if the blob is a valid FDT */
int  fdt_path(const char *path);                         /* "/chosen" */
int  fdt_first_child(int node);
int  fdt_next_sibling(int node);
int  fdt_find_compatible(const char *compat);            /* first node whose "compatible" list holds it */
int  fdt_find_compatible_after(int after, const char *compat);
const char *fdt_name(int node);
const void *fdt_prop(int node, const char *name, int *len);
uint32_t fdt_prop_u32(int node, const char *name, int index, uint32_t dflt);
uint64_t fdt_prop_u64(int node, const char *name, int index, uint64_t dflt);   /* two consecutive cells */
int  fdt_walk_next(int node);                            /* structure order, any depth */
const void *fdt_blob(void);
size_t fdt_size(void);

struct aarch64_platform {
    uint64_t mem_base, mem_size;
    uint64_t initrd_base, initrd_size;
    uint64_t uart_base; int uart_irq;
    uint64_t gicd_base, gicc_base;
    uint64_t ecam_base, ecam_size;
    uint64_t pci_mem_base, pci_mem_size, pci_io_base, pci_io_size;  /* CPU addresses of the windows */
    uint64_t pci_io_pci_base;                                       /* what the I/O window is called on the bus */
    uint32_t timer_freq;
    int pci_irq_base;           /* SPI of INTA at slot 0; the map swizzles from there */
    struct { uint8_t slot, pin; int irq; } pci_irq[32];             /* from interrupt-map */
    int pci_irq_count;
    uint8_t pci_irq_slot_mask;  /* interrupt-map-mask: which slot bits the map looks at */
    char model[48];
    uint64_t dtb_phys; uint32_t dtb_size; int dtb_ok;
};
extern struct aarch64_platform platform;
