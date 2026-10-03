/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A Qualcomm SoC's shared memory (SMEM): the heap the boot loaders, the
 * modem and the other remote processors keep their items in (socinfo, the
 * SMP2P and GLINK FIFOs a Wi-Fi bring-up talks through). Read-only for now:
 * smem_get() finds an item, /dev/smem lists what is there. Nothing is
 * allocated, so the hardware mutex that guards allocation is not taken.
 * Layout as Linux's drivers/soc/qcom/smem.c. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "endian.h"
#include "printf.h"
#include "string.h"
#include "abi/abi.h"

#define SMEM_GLOBAL_HOST        0xfffe
#define SMEM_ITEM_COUNT         512
#define SMEM_HW_SW_BUILD_ID     137         /* socinfo */
#define PRIVATE_CANARY          0xa5a5

static volatile uint8_t *smem;
static uint64_t smem_phys, smem_size;
static uint32_t version;                    /* 11: global heap, 12: global partition */

static uint32_t rd32(uint64_t off) { return off + 4 <= smem_size ? mmio_read32(smem + off) : 0; }
static uint16_t rd16(uint64_t off) { uint32_t v = rd32(off & ~3ull); return (uint16_t)(off & 2 ? v >> 16 : v); }

/* The header: proc_comm[4] (16 bytes each), version[32], initialized,
 * free_offset, available, reserved, then toc[512] of {allocated, offset,
 * size, aux_base}. The partition table sits in the last 4K. */
#define HDR_VERSION(i)  (0x40 + 4 * (i))
#define HDR_TOC(i)      (0xd0 + 16 * (i))
#define PTABLE          (smem_size - 4096)
#define PT_ENTRY(i)     (PTABLE + 32 + 40 * (i))  /* offset, size, flags, host0:16 host1:16, cacheline, reserved[7] */

static int ptable_entries(void)
{
    if (smem_size < 8192 || rd32(PTABLE) != 0x434f5424 /* "$TOC" */ || rd32(PTABLE + 4) != 1) return 0;
    uint32_t n = rd32(PTABLE + 8);
    return n > 64 ? 64 : (int)n;
}

/* Partition i's item `item` in its uncached list (from the front) or its
 * cached one (from the back, going down). */
static void *partition_item(int i, unsigned item, size_t *size)
{
    uint64_t off = rd32(PT_ENTRY(i)), psz = rd32(PT_ENTRY(i) + 4);
    uint32_t cl = rd32(PT_ENTRY(i) + 16);
    if (!off || off + psz > smem_size || rd32(off) != 0x54525024 /* "$PRT" */) return NULL;
    uint64_t free_unc = rd32(off + 12), free_cac = rd32(off + 16);
    for (uint64_t e = off + 32; e + 16 <= off + free_unc && rd16(e) == PRIVATE_CANARY; ) {
        uint32_t esz = rd32(e + 4);
        uint16_t pad_data = rd16(e + 8), pad_hdr = rd16(e + 10);
        if (rd16(e + 2) == item) {
            if (size) *size = esz - pad_data;
            return (void *)(smem + e + 16 + pad_hdr);
        }
        e += 16 + pad_hdr + esz;
    }
    if (!cl) cl = 64;
    uint64_t hdr = (16 + cl - 1) / cl * cl;
    for (uint64_t e = off + psz - hdr; e > off + free_cac && rd16(e) == PRIVATE_CANARY; ) {
        uint32_t esz = rd32(e + 4);
        if (rd16(e + 2) == item) {
            if (size) *size = esz - rd16(e + 8);
            return (void *)(smem + e - esz);
        }
        if (esz + hdr > e - off) break;
        e -= esz + hdr;
    }
    return NULL;
}

static int find_partition(unsigned host0, unsigned host1)
{
    int n = ptable_entries();
    for (int i = 0; i < n; i++) {
        uint32_t hosts = rd32(PT_ENTRY(i) + 12);
        unsigned a = hosts & 0xffff, b = hosts >> 16;
        if ((a == host0 && b == host1) || (a == host1 && b == host0)) return i;
    }
    return -1;
}

/* Item `item` of the partition `host` shares with the apps processor (host
 * 0), or of the global heap/partition for SMEM_GLOBAL_HOST or when there is
 * no private one. NULL if it was never allocated. */
void *smem_get(unsigned host, unsigned item, size_t *size)
{
    if (!smem || item >= SMEM_ITEM_COUNT) return NULL;
    int p = host != SMEM_GLOBAL_HOST ? find_partition(0, host) : -1;
    if (p >= 0) return partition_item(p, item, size);
    if (version == 12) {
        int n = ptable_entries();
        for (int i = 0; i < n; i++) {
            uint32_t hosts = rd32(PT_ENTRY(i) + 12);
            if ((hosts & 0xffff) == SMEM_GLOBAL_HOST && hosts >> 16 == SMEM_GLOBAL_HOST) return partition_item(i, item, size);
        }
        return NULL;
    }
    if (!rd32(HDR_TOC(item))) return NULL;
    uint32_t off = rd32(HDR_TOC(item) + 4), sz = rd32(HDR_TOC(item) + 8);
    if (off + (uint64_t)sz > smem_size) return NULL;
    if (size) *size = sz;
    return (void *)(smem + off);
}

uint64_t smem_virt_to_phys(const void *p) { return smem_phys + (uint64_t)((const volatile uint8_t *)p - smem); }

/* ---- /dev/smem --------------------------------------------------------------------- */

static char report[4096];

static size_t make_report(void)
{
    size_t n = 0;
#define P(...) do { if (n < sizeof report) n += (size_t)ksnprintf(report + n, sizeof report - n, __VA_ARGS__); } while (0)
    P("smem at %lx+%lx, version %u\n", (unsigned long)smem_phys, (unsigned long)smem_size, version);
    size_t sz;
    const volatile uint8_t *si = smem_get(SMEM_GLOBAL_HOST, SMEM_HW_SW_BUILD_ID, &sz);
    if (si && sz >= 44) {
        uint32_t fmt = mmio_read32(si), id = mmio_read32(si + 4), ver = mmio_read32(si + 8);
        char build[33];
        for (int i = 0; i < 32; i++) build[i] = (char)si[12 + i];
        build[32] = 0;
        P("socinfo: format %u.%u, soc id %u, version %u.%u, build \"%s\"\n", fmt >> 16, fmt & 0xffff, id, ver >> 16, ver & 0xffff, build);
    }
    int pt = ptable_entries();
    for (int i = 0; i < pt; i++) {
        uint32_t hosts = rd32(PT_ENTRY(i) + 12);
        P("partition %d: hosts %x/%x, %u bytes at %x:", i, hosts & 0xffff, hosts >> 16, rd32(PT_ENTRY(i) + 4), rd32(PT_ENTRY(i)));
        for (unsigned item = 0; item < SMEM_ITEM_COUNT; item++) {
            size_t isz;
            if (partition_item(i, item, &isz)) P(" %u(%lu)", item, (unsigned long)isz);
        }
        P("\n");
    }
    if (version != 12) {
        P("global heap:");
        for (unsigned item = 0; item < SMEM_ITEM_COUNT; item++)
            if (rd32(HDR_TOC(item))) P(" %u(%u)", item, rd32(HDR_TOC(item) + 8));
        P("\n");
    }
#undef P
    if (n >= sizeof report) n = sizeof report - 1;
    return n;
}

static long smem_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) make_report();
    size_t n = strlen(report);
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static const struct dev_ops smem_ops = { .read = smem_read };

void qcom_smem_init(void)
{
    int n = fdt_find_compatible("qcom,smem");
    uint32_t ph = n >= 0 ? fdt_prop_u32(n, "memory-region", 0, 0) : 0;
    for (int m = fdt_path("/"); ph && m >= 0; m = fdt_walk_next(m))
        if (fdt_prop_u32(m, "phandle", 0, 0) == ph) { fdt_reg(m, 0, &smem_phys, &smem_size); break; }
    if (!smem_phys || smem_size < 0x2000) return;
    smem = vmm_map_wc(smem_phys, smem_size);
    if (!smem) return;
    version = rd32(HDR_VERSION(7)) >> 16;       /* SMEM_MASTER_SBL_VERSION_INDEX */
    if (version != 11 && version != 12) {
        kprintf("smem: unknown version %08x at %lx\n", rd32(HDR_VERSION(7)), (unsigned long)smem_phys);
        smem = NULL;
        return;
    }
    vfs_mkdev("/dev/smem", &smem_ops, NULL);
    kprintf("smem: version %u, %d partitions\n", version, ptable_entries());
}
