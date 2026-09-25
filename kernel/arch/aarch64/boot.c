/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Early boot: read the device tree, build the boot info the generic
 * kernel expects, call kernel_main. head.S has already switched the MMU on
 * with the direct map, so everything is reachable through P2V. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/cpu.h"
#include "asm/sysreg.h"
#include "zaeboot.h"
#include "string.h"

struct aarch64_platform platform;

extern void kernel_main(struct zaeboot_info *info);
extern char _kernel_start[], _kernel_end[];

static struct zaeboot_info boot_info __attribute__((aligned(16)));
static struct zaeboot_mmap_entry boot_mmap[16];

static void add_mmap(uint64_t base, uint64_t len, uint32_t type)
{
    if (boot_info.mmap_count >= 16 || len == 0) return;
    boot_mmap[boot_info.mmap_count].base = base;
    boot_mmap[boot_info.mmap_count].length = len;
    boot_mmap[boot_info.mmap_count].type = type;
    boot_info.mmap_count++;
}

/* The node a phandle names: every node in structure order. */
static int node_by_phandle(uint32_t ph)
{
    for (int n = fdt_path("/"); n >= 0; n = fdt_walk_next(n))
        if (fdt_prop_u32(n, "phandle", 0, 0) == ph || fdt_prop_u32(n, "linux,phandle", 0, 0) == ph) return n;
    return -1;
}

static void scan_pcie(int pci)
{
    platform.ecam_base = fdt_prop_u64(pci, "reg", 0, 0);
    platform.ecam_size = fdt_prop_u64(pci, "reg", 2, 0);
    int len;
    const uint8_t *r = fdt_prop(pci, "ranges", &len);
    /* (pci.hi pci.mid pci.lo) (cpu: 2 cells) (size: 2 cells) */
    for (int i = 0; r && i + 28 <= len; i += 28) {
        uint32_t hi = (uint32_t)r[i] << 24 | (uint32_t)r[i + 1] << 16 | (uint32_t)r[i + 2] << 8 | r[i + 3];
        uint64_t pci_addr = 0, cpu = 0, size = 0;
        for (int k = 0; k < 8; k++) { pci_addr = pci_addr << 8 | r[i + 4 + k]; cpu = cpu << 8 | r[i + 12 + k]; size = size << 8 | r[i + 20 + k]; }
        int space = (hi >> 24) & 3;
        if (space == 1 && !platform.pci_io_base) { platform.pci_io_base = cpu; platform.pci_io_size = size; platform.pci_io_pci_base = pci_addr; }
        else if (space == 2 && !platform.pci_mem_base) { platform.pci_mem_base = cpu; platform.pci_mem_size = size; }
    }
    /* interrupt-map: (child addr: 3) (child irq: 1) (phandle: 1) (parent addr: N) (parent irq: 3) */
    const uint8_t *m = fdt_prop(pci, "interrupt-map", &len);
    if (!m) return;
    uint32_t ph = (uint32_t)m[16] << 24 | (uint32_t)m[17] << 16 | (uint32_t)m[18] << 8 | m[19];
    int parent = node_by_phandle(ph);
    int pac = parent >= 0 ? (int)fdt_prop_u32(parent, "#address-cells", 0, 0) : 0;
    int pic = parent >= 0 ? (int)fdt_prop_u32(parent, "#interrupt-cells", 0, 3) : 3;
    int entry = (3 + 1 + 1 + pac + pic) * 4;
    uint32_t mask_hi = 0xf800;
    const uint8_t *mk = fdt_prop(pci, "interrupt-map-mask", NULL);
    if (mk) mask_hi = (uint32_t)mk[0] << 24 | (uint32_t)mk[1] << 16 | (uint32_t)mk[2] << 8 | mk[3];
    platform.pci_irq_slot_mask = (uint8_t)((mask_hi >> 11) & 31);
    for (int i = 0; i + entry <= len && platform.pci_irq_count < 32; i += entry) {
        uint32_t chi = (uint32_t)m[i] << 24 | (uint32_t)m[i + 1] << 16 | (uint32_t)m[i + 2] << 8 | m[i + 3];
        uint32_t pin = m[i + 15];
        const uint8_t *p = m + i + (5 + pac) * 4;
        uint32_t type = p[3], num = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
        int k = platform.pci_irq_count++;
        platform.pci_irq[k].slot = (uint8_t)(((chi & mask_hi) >> 11) & 31);
        platform.pci_irq[k].pin = (uint8_t)pin;
        platform.pci_irq[k].irq = (int)(type == 0 ? 32 + num : type == 1 ? 16 + num : num);
    }
}

static void scan_tree(void)
{
    int root = fdt_path("/");
    int len;
    const char *model = fdt_prop(root, "model", &len);
    if (model) { size_t n = strlen(model); if (n > sizeof platform.model - 1) n = sizeof platform.model - 1; memcpy(platform.model, model, n); }

    int mem = fdt_path("/memory");
    if (mem < 0) mem = fdt_find_compatible("memory");
    for (int c = fdt_first_child(root); mem < 0 && c >= 0; c = fdt_next_sibling(c))
        if (memcmp(fdt_name(c), "memory", 6) == 0) mem = c;
    platform.mem_base = fdt_prop_u64(mem, "reg", 0, 0x40000000);
    platform.mem_size = fdt_prop_u64(mem, "reg", 2, 128u << 20);

    int chosen = fdt_path("/chosen");
    const uint8_t *p = fdt_prop(chosen, "linux,initrd-start", &len);
    if (p) {
        platform.initrd_base = len == 8 ? fdt_prop_u64(chosen, "linux,initrd-start", 0, 0) : fdt_prop_u32(chosen, "linux,initrd-start", 0, 0);
        uint64_t end = 0;
        p = fdt_prop(chosen, "linux,initrd-end", &len);
        if (p) end = len == 8 ? fdt_prop_u64(chosen, "linux,initrd-end", 0, 0) : fdt_prop_u32(chosen, "linux,initrd-end", 0, 0);
        platform.initrd_size = end > platform.initrd_base ? end - platform.initrd_base : 0;
    }

    int uart = fdt_find_compatible("arm,pl011");
    platform.uart_base = fdt_prop_u64(uart, "reg", 0, 0x09000000);
    platform.uart_irq = uart >= 0 ? (int)(32 + fdt_prop_u32(uart, "interrupts", 1, 1)) : 33;

    int gic = fdt_find_compatible("arm,cortex-a15-gic");
    if (gic < 0) gic = fdt_find_compatible("arm,gic-400");
    platform.gicd_base = fdt_prop_u64(gic, "reg", 0, 0x08000000);
    platform.gicc_base = fdt_prop_u64(gic, "reg", 4, 0x08010000);

    platform.timer_freq = (uint32_t)read_sysreg(cntfrq_el0);
    int timer = fdt_find_compatible("arm,armv8-timer");
    if (timer >= 0) platform.timer_freq = fdt_prop_u32(timer, "clock-frequency", 0, platform.timer_freq);

    int pci = fdt_find_compatible("pci-host-ecam-generic");
    if (pci >= 0) scan_pcie(pci);
}

void aarch64_early_main(uint64_t dtb_phys)
{
    memset(&platform, 0, sizeof platform);
    platform.dtb_phys = dtb_phys;
    /* The blob stays where the loader put it (QEMU pads it to 1 MiB) and
     * is kept out of the allocator below. */
    platform.dtb_ok = dtb_phys && fdt_init(P2V(dtb_phys)) == 0;
    if (platform.dtb_ok) { platform.dtb_size = (uint32_t)fdt_size(); scan_tree(); }
    else {
        platform.mem_base = 0x40000000; platform.mem_size = 128u << 20;
        platform.uart_base = 0x09000000; platform.uart_irq = 33;
        platform.gicd_base = 0x08000000; platform.gicc_base = 0x08010000;
        platform.timer_freq = (uint32_t)read_sysreg(cntfrq_el0);
    }

    memset(&boot_info, 0, sizeof boot_info);
    boot_info.magic = ZAEBOOT_MAGIC;
    boot_info.version = ZAEBOOT_VERSION;
    boot_info.size = sizeof boot_info;
    boot_info.firmware = ZAEBOOT_FW_DEVICETREE;
    uint64_t kphys = V2P(_kernel_start), kend = (V2P(_kernel_end) + 0xFFF) & ~0xFFFULL;
    boot_info.kernel_phys_base = kphys;
    boot_info.kernel_size = kend - kphys;
    boot_info.initrd_addr = platform.initrd_base;
    boot_info.initrd_size = platform.initrd_size;
    uint64_t mem_end = platform.mem_base + platform.mem_size;
    if (mem_end > HHDM_SIZE) mem_end = HHDM_SIZE;          /* only what the direct map reaches */
    /* RAM minus what is already in it: the kernel, the initrd, the device
     * tree. Sorted, then the gaps between them are the usable memory. */
    struct { uint64_t s, e; uint32_t type; } res[3];
    int n = 0;
    res[n].s = kphys; res[n].e = kend; res[n++].type = ZAEBOOT_MEM_BOOTLOADER;
    if (platform.initrd_size) {
        res[n].s = platform.initrd_base & ~0xFFFULL; res[n].e = (platform.initrd_base + platform.initrd_size + 0xFFF) & ~0xFFFULL;
        res[n++].type = ZAEBOOT_MEM_BOOTLOADER;
    }
    if (platform.dtb_ok) {
        res[n].s = dtb_phys & ~0xFFFULL; res[n].e = (dtb_phys + platform.dtb_size + 0xFFF) & ~0xFFFULL;
        res[n++].type = ZAEBOOT_MEM_RESERVED;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && res[j].s < res[j - 1].s; j--) { __typeof__(res[0]) t = res[j]; res[j] = res[j - 1]; res[j - 1] = t; }
    uint64_t at = platform.mem_base;
    for (int i = 0; i < n; i++) {
        if (res[i].s > at) add_mmap(at, res[i].s - at, ZAEBOOT_MEM_USABLE);
        add_mmap(res[i].s, res[i].e - res[i].s, res[i].type);
        if (res[i].e > at) at = res[i].e;
    }
    if (mem_end > at) add_mmap(at, mem_end - at, ZAEBOOT_MEM_USABLE);
    boot_info.mmap = (uint64_t)(uintptr_t)boot_mmap;

    cpus[0].self = &cpus[0];
    cpus[0].index = 0;
    cpu_count = 1;
    kernel_main(&boot_info);
    for (;;) ;
}
