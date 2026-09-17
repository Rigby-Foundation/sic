/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* "Port" I/O on a PowerMac is the PCI I/O space window of the host bridge,
 * little-endian like everything on PCI. io_base is set by the arch init. */
#pragma once
#include "types.h"
#include "endian.h"

extern volatile uint8_t *ppc_io_base;

static inline void outb(uint16_t port, uint8_t v)   { mmio_write8(ppc_io_base + port, v); __asm__ volatile("eieio"); }
static inline uint8_t inb(uint16_t port)            { uint8_t v = mmio_read8(ppc_io_base + port); __asm__ volatile("eieio"); return v; }
static inline void outw(uint16_t port, uint16_t v)  { mmio_write16(ppc_io_base + port, v); __asm__ volatile("eieio"); }
static inline uint16_t inw(uint16_t port)           { uint16_t v = mmio_read16(ppc_io_base + port); __asm__ volatile("eieio"); return v; }
static inline void outl(uint16_t port, uint32_t v)  { mmio_write32(ppc_io_base + port, v); __asm__ volatile("eieio"); }
static inline uint32_t inl(uint16_t port)           { uint32_t v = mmio_read32(ppc_io_base + port); __asm__ volatile("eieio"); return v; }
