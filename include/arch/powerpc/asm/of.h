/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* OpenFirmware client interface, for the early boot code only. */
#pragma once
#include "types.h"

void     of_init(void *entry);
uint32_t of_finddevice(const char *path);
int      of_getprop(uint32_t ph, const char *name, void *buf, uint32_t len);
uint32_t of_getprop_u32(uint32_t ph, const char *name, uint32_t dflt);
uint32_t of_child(uint32_t ph);
uint32_t of_peer(uint32_t ph);
uint32_t of_parent(uint32_t ph);
uint32_t of_instance_to_package(uint32_t ih);
int      of_package_to_path(uint32_t ph, char *buf, uint32_t len);
int      of_nextprop(uint32_t ph, const char *prev, char *buf);   /* 32-byte buf; 0 = end */
void     of_write(uint32_t ih, const char *s, uint32_t len);
uint32_t of_claim(uint32_t addr, uint32_t size, uint32_t align);
void     of_quiesce(void);
void     of_exit(void) __attribute__((noreturn));
uint32_t of_find_by_name(uint32_t root, const char *name);
uint32_t of_find_by_type(uint32_t root, const char *type);

/* What the early code learned, for the rest of the arch code. */
struct ppc_platform {
    uint32_t mem_size;              /* bytes of RAM (contiguous from 0) */
    uint32_t timebase_freq;         /* Hz */
    uint32_t cpu_freq;
    uint32_t macio_base;            /* physical: ESCC, IDE, OpenPIC live here */
    uint32_t openpic_base;          /* physical */
    uint32_t ide_offset[4];         /* ATA cells ("ata-*" nodes) as offsets into mac-io */
    int      ide_count;
    int      escc_irq;              /* OpenPIC source of ESCC channel A, or -1 */
    int      escc_irq_level;        /* its sense: 1 = level, 0 = edge */
    uint32_t pci_cfg_base[3];       /* Uni-North host bridge register bases */
    uint32_t pci_io_base;           /* PCI I/O space window, physical */
    struct { uint8_t bus, slot, func; int irq; } pci_irq[32];   /* "interrupts" of the PCI nodes */
    int      pci_irq_count;
    uint32_t fb_base, fb_width, fb_height, fb_pitch, fb_depth;
    uint32_t initrd_base, initrd_size;
    uint32_t stdout_ih;
    int      is_qemu;               /* firmware is OpenBIOS: a VM, disks may be scratch */
};
extern struct ppc_platform platform;
