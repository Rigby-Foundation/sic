/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * zaeboot -> sic boot protocol.
 * Keep in sync with zaeboot/src/protocol.zig.
 *
 * The bootloader calls the kernel's ELF entry point in 64-bit long mode with
 * interrupts disabled, paging enabled with all of physical memory identity
 * mapped (as UEFI left it), and a pointer to `struct zaeboot_info` in RDI
 * (System V ABI). The kernel is responsible for its own stack, GDT and IDT.
 */
#pragma once
#include "types.h"

#define ZAEBOOT_MAGIC   0x00544F4F4245415AULL   /* "ZAEBOOT\0" */
#define ZAEBOOT_VERSION 3

enum zaeboot_mem_type {
    ZAEBOOT_MEM_USABLE           = 1,
    ZAEBOOT_MEM_RESERVED         = 2,
    ZAEBOOT_MEM_ACPI_RECLAIMABLE = 3,
    ZAEBOOT_MEM_ACPI_NVS         = 4,
    ZAEBOOT_MEM_BOOTLOADER       = 5,   /* loader, boot info, kernel image (see kernel_phys_base) */
    ZAEBOOT_MEM_BAD              = 6,
    ZAEBOOT_MEM_BOOT_SERVICES    = 7,   /* firmware boot-services memory; usable once the kernel
                                           runs on its own page tables and stack */
};

struct zaeboot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;      /* enum zaeboot_mem_type */
    uint32_t reserved;
} __attribute__((packed));

struct zaeboot_framebuffer {
    uint64_t base;      /* physical address */
    uint32_t width;     /* pixels */
    uint32_t height;    /* pixels */
    uint32_t pitch;     /* bytes per scanline */
    uint32_t bpp;       /* bits per pixel (32) */
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  reserved[5];
} __attribute__((packed));

struct zaeboot_info {
    uint64_t magic;
    uint32_t version;
    uint32_t size;      /* sizeof(struct zaeboot_info) */

    struct zaeboot_framebuffer fb;

    uint64_t mmap;      /* physical address of struct zaeboot_mmap_entry[] */
    uint64_t mmap_count;

    uint64_t rsdp;      /* ACPI RSDP physical address, 0 if none */

    uint64_t kernel_phys_base;
    uint64_t kernel_size;

    /* v2+: optional initial ramdisk (USTAR), 0/0 if absent. Check `size`. */
    uint64_t initrd_addr;
    uint64_t initrd_size;

    /* v3+: which firmware we came from (ZAEBOOT_FW_*). */
    uint32_t firmware;
    uint32_t reserved;
} __attribute__((packed));

#define ZAEBOOT_FW_UNKNOWN 0
#define ZAEBOOT_FW_UEFI    1
#define ZAEBOOT_FW_BIOS    2
#define ZAEBOOT_FW_OPENFIRMWARE 3   /* powerpc: the kernel built the info itself from the device tree */
#define ZAEBOOT_FW_DEVICETREE 4     /* aarch64: likewise, from a flattened device tree */
