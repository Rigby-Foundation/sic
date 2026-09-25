/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* "Port" I/O is the PCI I/O window of the host bridge (QEMU virt: 64 KiB
 * at 0x3eff0000), little-endian like everything on PCI. */
#pragma once
#include "types.h"
#include "endian.h"

extern volatile uint8_t *aarch64_io_base;

static inline void outb(uint16_t port, uint8_t v)   { if (aarch64_io_base) mmio_write8(aarch64_io_base + port, v); }
static inline uint8_t inb(uint16_t port)            { return aarch64_io_base ? mmio_read8(aarch64_io_base + port) : 0xFF; }
static inline void outw(uint16_t port, uint16_t v)  { if (aarch64_io_base) mmio_write16(aarch64_io_base + port, v); }
static inline uint16_t inw(uint16_t port)           { return aarch64_io_base ? mmio_read16(aarch64_io_base + port) : 0xFFFF; }
static inline void outl(uint16_t port, uint32_t v)  { if (aarch64_io_base) mmio_write32(aarch64_io_base + port, v); }
static inline uint32_t inl(uint16_t port)           { return aarch64_io_base ? mmio_read32(aarch64_io_base + port) : 0xFFFFFFFF; }
