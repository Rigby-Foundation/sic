/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Byte order. Everything on a disk (zaefs, FAT, MBR/GPT), on the PCI bus
 * (device registers, DMA descriptors) is little-endian; everything on the
 * network is big-endian. The CPU may be either (x86_64: LE, powerpc: BE),
 * so such fields are only ever read and written through these helpers. */
#pragma once
#include "types.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SIC_LITTLE_ENDIAN 1
#define SIC_BIG_ENDIAN_CPU 0
#else
#define SIC_BIG_ENDIAN 1
#define SIC_BIG_ENDIAN_CPU 1
#endif

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

#ifdef SIC_LITTLE_ENDIAN
static inline uint16_t le16toh(uint16_t v) { return v; }
static inline uint32_t le32toh(uint32_t v) { return v; }
static inline uint64_t le64toh(uint64_t v) { return v; }
static inline uint16_t htole16(uint16_t v) { return v; }
static inline uint32_t htole32(uint32_t v) { return v; }
static inline uint64_t htole64(uint64_t v) { return v; }
static inline uint16_t be16toh(uint16_t v) { return bswap16(v); }
static inline uint32_t be32toh(uint32_t v) { return bswap32(v); }
static inline uint64_t be64toh(uint64_t v) { return bswap64(v); }
static inline uint16_t htobe16(uint16_t v) { return bswap16(v); }
static inline uint32_t htobe32(uint32_t v) { return bswap32(v); }
static inline uint64_t htobe64(uint64_t v) { return bswap64(v); }
#else
static inline uint16_t le16toh(uint16_t v) { return bswap16(v); }
static inline uint32_t le32toh(uint32_t v) { return bswap32(v); }
static inline uint64_t le64toh(uint64_t v) { return bswap64(v); }
static inline uint16_t htole16(uint16_t v) { return bswap16(v); }
static inline uint32_t htole32(uint32_t v) { return bswap32(v); }
static inline uint64_t htole64(uint64_t v) { return bswap64(v); }
static inline uint16_t be16toh(uint16_t v) { return v; }
static inline uint32_t be32toh(uint32_t v) { return v; }
static inline uint64_t be64toh(uint64_t v) { return v; }
static inline uint16_t htobe16(uint16_t v) { return v; }
static inline uint32_t htobe32(uint32_t v) { return v; }
static inline uint64_t htobe64(uint64_t v) { return v; }
#endif

/* Unaligned little-endian fields in on-disk structures. */
static inline uint16_t get_le16(const void *p) { const uint8_t *b = p; return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t get_le32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static inline uint64_t get_le64(const void *p) { const uint8_t *b = p; return (uint64_t)get_le32(b) | (uint64_t)get_le32(b + 4) << 32; }
static inline void put_le16(void *p, uint16_t v) { uint8_t *b = p; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void put_le32(void *p, uint32_t v) { uint8_t *b = p; for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static inline void put_le64(void *p, uint64_t v) { uint8_t *b = p; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }

/* Ordering against devices. x86 keeps program order for uncached I/O and
 * DMA-visible stores; PowerPC needs explicit fences: sync before telling a
 * device about memory we just wrote (dma_wmb), lwsync/isync after reading
 * status before the data (dma_rmb), eieio around every register access. */
#ifdef __powerpc__
#define dma_wmb()   __asm__ volatile("sync" ::: "memory")
#define dma_rmb()   __asm__ volatile("lwsync" ::: "memory")
#define mmio_fence() __asm__ volatile("eieio" ::: "memory")
#elif defined(__aarch64__)
#define dma_wmb()   __asm__ volatile("dsb sy" ::: "memory")
#define dma_rmb()   __asm__ volatile("dsb sy" ::: "memory")
#define mmio_fence() __asm__ volatile("dmb sy" ::: "memory")
#else
#define dma_wmb()   __asm__ volatile("" ::: "memory")
#define dma_rmb()   __asm__ volatile("" ::: "memory")
#define mmio_fence() __asm__ volatile("" ::: "memory")
#endif

/* Little-endian device registers and DMA descriptors (PCI is little-endian
 * regardless of the CPU). Reads/writes are volatile and ordered. */
static inline uint32_t mmio_read32(const volatile void *p) { mmio_fence(); uint32_t v = *(const volatile uint32_t *)p; mmio_fence(); return le32toh(v); }
static inline uint16_t mmio_read16(const volatile void *p) { mmio_fence(); uint16_t v = *(const volatile uint16_t *)p; mmio_fence(); return le16toh(v); }
static inline uint8_t  mmio_read8(const volatile void *p)  { mmio_fence(); uint8_t v = *(const volatile uint8_t *)p; mmio_fence(); return v; }
static inline uint64_t mmio_read64(const volatile void *p) { return (uint64_t)mmio_read32(p) | (uint64_t)mmio_read32((const volatile uint8_t *)p + 4) << 32; }
static inline void mmio_write32(volatile void *p, uint32_t v) { mmio_fence(); *(volatile uint32_t *)p = htole32(v); mmio_fence(); }
static inline void mmio_write16(volatile void *p, uint16_t v) { mmio_fence(); *(volatile uint16_t *)p = htole16(v); mmio_fence(); }
static inline void mmio_write8(volatile void *p, uint8_t v)   { mmio_fence(); *(volatile uint8_t *)p = v; mmio_fence(); }
static inline void mmio_write64(volatile void *p, uint64_t v) { mmio_write32(p, (uint32_t)v); mmio_write32((volatile uint8_t *)p + 4, (uint32_t)(v >> 32)); }
