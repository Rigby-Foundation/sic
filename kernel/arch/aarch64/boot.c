/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Early boot: read the device tree, build the boot info the generic
 * kernel expects, call kernel_main. head.S has already switched the MMU on
 * with a rough direct map (1 GiB blocks); once the tree says where RAM is
 * and which parts of it firmware keeps, a precise one replaces it: on a
 * phone, even a speculative read of the hypervisor's or the modem's
 * memory can reset the machine. A Qualcomm bootloader also leaves its
 * splash screen up, which becomes the console, and puts its own ramdisk
 * before ours. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/cpu.h"
#include "asm/sysreg.h"
#include "zaeboot.h"
#include "drivers/fb.h"
#include "string.h"

struct aarch64_platform platform;

extern void kernel_main(struct zaeboot_info *info);
extern char _kernel_start[], _kernel_end[];

static struct zaeboot_info boot_info __attribute__((aligned(16)));
#define MMAP_MAX 160
static struct zaeboot_mmap_entry boot_mmap[MMAP_MAX];

static void add_mmap(uint64_t base, uint64_t len, uint32_t type)
{
    if (boot_info.mmap_count >= MMAP_MAX || len == 0) return;
    boot_mmap[boot_info.mmap_count].base = base;
    boot_mmap[boot_info.mmap_count].length = len;
    boot_mmap[boot_info.mmap_count].type = type;
    boot_info.mmap_count++;
}

/* ---- intervals of physical memory --------------------------------------------- */

struct iv { uint64_t s, e; };

/* Take [s, e) out of the list (kept sorted and disjoint). */
static int iv_subtract(struct iv *v, int n, int cap, uint64_t s, uint64_t e)
{
    for (int i = 0; i < n && s < e; i++) {
        if (e <= v[i].s || s >= v[i].e) continue;
        if (s > v[i].s && e < v[i].e) {                 /* a hole in the middle: split */
            if (n == cap) { v[i].e = s; continue; }     /* no room: lose the top part */
            for (int k = n; k > i + 1; k--) v[k] = v[k - 1];
            v[i + 1].s = e; v[i + 1].e = v[i].e;
            v[i].e = s;
            n++;
            i++;
        } else if (s <= v[i].s && e >= v[i].e) {       /* all of it */
            for (int k = i; k < n - 1; k++) v[k] = v[k + 1];
            n--;
            i--;
        } else if (s <= v[i].s) v[i].s = e;
        else v[i].e = s;
    }
    return n;
}

/* ---- the initrd: ours may follow the bootloader's ------------------------------- */

static int tar_header_ok(const uint8_t *h)
{
    if (memcmp(h + 257, "ustar", 5) != 0) return 0;
    unsigned sum = 0, want = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    for (int i = 148; i < 156 && h[i] >= '0' && h[i] <= '7'; i++) want = want * 8 + (unsigned)(h[i] - '0');
    return sum == want;
}

/* A Qualcomm bootloader hands over its vendor ramdisk with the boot
 * image's ramdisk (our tar) appended: find the tar, and move it to a page
 * boundary so its contents are aligned the way they were packed. */
uint64_t initrd_given_base, initrd_given_size;      /* what the loader said, for the report */
int64_t initrd_tar_at = -1;                         /* where in it our tar starts (-1: not found) */
int64_t initrd_ustar_at = -1;                       /* the first "ustar" seen, checksum or not */

static void find_tar_in_initrd(void)
{
    uint8_t *p = P2V(platform.initrd_base);
    initrd_given_base = platform.initrd_base;
    initrd_given_size = platform.initrd_size;
    if (platform.initrd_size >= 512 && tar_header_ok(p)) { initrd_tar_at = 0; return; }
    if (platform.initrd_size < 1024) return;
    for (uint64_t off = 0; off + 5 <= platform.initrd_size && initrd_ustar_at < 0; off++)
        if (p[off] == 'u' && memcmp(p + off, "ustar", 5) == 0) initrd_ustar_at = (int64_t)off;
    for (uint64_t off = 1; off + 512 <= platform.initrd_size; off++) {
        if (p[off + 257] != 'u' || !tar_header_ok(p + off)) continue;
        initrd_tar_at = (int64_t)off;
        uint64_t len = platform.initrd_size - off, dst = (platform.initrd_base + off) & ~0xFFFULL;
        if (dst < platform.initrd_base) dst = platform.initrd_base + off;   /* (cannot happen past a 4 KiB ramdisk) */
        memmove(P2V(dst), p + off, len);
        platform.initrd_base = dst;
        platform.initrd_size = len;
        return;
    }
}

/* ---- the precise direct map ------------------------------------------------------ */

/* [0, 1 GiB) stays device memory in one block (every register of these
 * boards lives there); [1, 4 GiB) maps RAM only, and none of firmware's
 * no-map regions: 2 MiB blocks where a block is all one kind, 4 KiB pages
 * where an edge falls inside. The splash screen is mapped uncached. */
#define DM_L3_MAX 256
static uint64_t dm_l0[512] __attribute__((aligned(4096), section(".bss.pagetables")));
static uint64_t dm_l1[512] __attribute__((aligned(4096), section(".bss.pagetables")));
static uint64_t dm_l2[3][512] __attribute__((aligned(4096), section(".bss.pagetables")));
static uint64_t dm_l3[DM_L3_MAX][512] __attribute__((aligned(4096), section(".bss.pagetables")));
uint64_t kernel_ttbr1;              /* the kernel half's level-0 table, physical: mmu.c, and head.S for other CPUs */

static struct iv dm_ram[MMAP_MAX];
static int dm_nram;
static uint64_t dm_fb_s, dm_fb_e;

static int dm_kind(uint64_t pa)     /* 0 unmapped, 1 normal, 2 normal non-cacheable */
{
    if (pa >= dm_fb_s && pa < dm_fb_e) return 2;
    for (int i = 0; i < dm_nram; i++) if (pa >= dm_ram[i].s && pa < dm_ram[i].e) return 1;
    return 0;
}

static int dm_edge_inside(uint64_t s, uint64_t e)
{
    if (dm_fb_e && ((dm_fb_s > s && dm_fb_s < e) || (dm_fb_e > s && dm_fb_e < e))) return 1;
    for (int i = 0; i < dm_nram; i++)
        if ((dm_ram[i].s > s && dm_ram[i].s < e) || (dm_ram[i].e > s && dm_ram[i].e < e)) return 1;
    return 0;
}

static uint64_t dm_desc(uint64_t pa, int kind, int page)
{
    uint64_t d = pa | (page ? 3 : 1) | (1UL << 10) | (3UL << 8);   /* valid, AF, inner shareable */
    if (kind == 2) d |= 3UL << 2;                                 /* MAIR 3: normal non-cacheable */
    return d;
}

static void build_direct_map(void)
{
    dm_nram = 0;
    for (int i = 0; i < platform.nmem && dm_nram < MMAP_MAX; i++) {
        uint64_t s0 = platform.mem[i].base, e0 = platform.mem[i].base + platform.mem[i].size;
        if (e0 > HHDM_SIZE) e0 = HHDM_SIZE;
        if (s0 < e0) { dm_ram[dm_nram].s = s0; dm_ram[dm_nram].e = e0; dm_nram++; }
    }
    for (int i = 0; i < platform.nrsv; i++)
        if (platform.rsv[i].nomap)
            dm_nram = iv_subtract(dm_ram, dm_nram, MMAP_MAX, platform.rsv[i].base, platform.rsv[i].base + platform.rsv[i].size);
    if (platform.fb_base) {
        dm_fb_s = platform.fb_base & ~0xFFFULL;
        dm_fb_e = (platform.fb_base + (uint64_t)platform.fb_pitch * platform.fb_height + 0xFFF) & ~0xFFFULL;
    }
    dm_l0[0] = V2P(dm_l1) | 3;
    dm_l1[0] = 0x401 | (1 << 2);                    /* [0, 1 GiB): device, as head.S had it */
    int nl3 = 0;
    for (uint64_t gb = 1; gb < 4; gb++) {
        uint64_t *l2 = dm_l2[gb - 1];
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t pa = gb << 30 | i << 21;
            if (!dm_edge_inside(pa, pa + (2UL << 20)) || nl3 == DM_L3_MAX) {
                int k = dm_kind(pa);
                if (k) l2[i] = dm_desc(pa, k, 0);
                continue;
            }
            uint64_t *l3 = dm_l3[nl3++];
            for (uint64_t j = 0; j < 512; j++) {
                uint64_t p = pa + j * 4096;
                int k = dm_kind(p);
                l3[j] = k ? dm_desc(p, k, 1) : 0;
            }
            l2[i] = V2P(l3) | 3;
        }
        dm_l1[gb] = V2P(l2) | 3;
    }
    kernel_ttbr1 = V2P(dm_l0);
    __asm__ volatile("dsb ishst; msr ttbr1_el1, %0; isb; tlbi vmalle1is; dsb ish; isb" : : "r"(kernel_ttbr1) : "memory");
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

/* RAM: every memory node, every range in it. Firmware's regions: the
 * children of /reserved-memory that have an address ("no-map" ones are
 * never to be touched, not even mapped) and the header's /memreserve/s. */
static void scan_memory(int root)
{
    for (int n = fdt_first_child(root); n >= 0; n = fdt_next_sibling(n)) {
        const char *type = fdt_prop(n, "device_type", NULL);
        if (!(type && strcmp(type, "memory") == 0) && !(memcmp(fdt_name(n), "memory", 6) == 0 && (fdt_name(n)[6] == 0 || fdt_name(n)[6] == '@')))
            continue;
        uint64_t b, sz;
        for (int i = 0; fdt_reg(n, i, &b, &sz) == 0 && platform.nmem < 32; i++)
            if (sz) { platform.mem[platform.nmem].base = b; platform.mem[platform.nmem].size = sz; platform.nmem++; }
    }
    if (!platform.nmem) { platform.mem[0].base = 0x40000000; platform.mem[0].size = 128u << 20; platform.nmem = 1; }
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < platform.nmem; i++) {
        if (platform.mem[i].base < lo) lo = platform.mem[i].base;
        if (platform.mem[i].base + platform.mem[i].size > hi) hi = platform.mem[i].base + platform.mem[i].size;
    }
    platform.mem_base = lo;
    platform.mem_size = hi - lo;

    int rm = fdt_path("/reserved-memory");
    for (int c = fdt_first_child(rm); c >= 0; c = fdt_next_sibling(c)) {
        uint64_t b, sz;
        int nomap = fdt_prop(c, "no-map", NULL) != NULL;
        for (int i = 0; fdt_reg(c, i, &b, &sz) == 0 && platform.nrsv < 96; i++)
            if (sz) { platform.rsv[platform.nrsv].base = b; platform.rsv[platform.nrsv].size = sz; platform.rsv[platform.nrsv].nomap = nomap; platform.nrsv++; }
    }
    uint64_t b, sz;
    for (int i = 0; fdt_memreserve(i, &b, &sz) && platform.nrsv < 96; i++) {
        platform.rsv[platform.nrsv].base = b; platform.rsv[platform.nrsv].size = sz; platform.rsv[platform.nrsv].nomap = 0; platform.nrsv++;
    }
}

/* ---- the splash screen a Qualcomm bootloader leaves up ------------------------- */

/* The display controller is still scanning out the bootloader's buffer:
 * its source pipes say where that is and what it looks like. */
#define SSPP_SRC_SIZE      0x00
#define SSPP_SRC_IMG_SIZE  0x04
#define SSPP_SRC0_ADDR     0x14
#define SSPP_SRC_YSTRIDE0  0x24
#define SSPP_SRC_FORMAT    0x30
static void probe_qcom_splash(void)
{
    int mdp = fdt_find_compatible("qcom,sde-kms");
    uint64_t base;
    if (mdp < 0 || fdt_reg(mdp, 0, &base, NULL) != 0 || base >= 0x40000000) return;   /* the device part of the map */
    int len;
    const uint8_t *offs = fdt_prop(mdp, "qcom,sde-sspp-off", &len);
    for (int i = 0; offs && i + 4 <= len; i += 4) {
        uint32_t off = (uint32_t)offs[i] << 24 | (uint32_t)offs[i + 1] << 16 | (uint32_t)offs[i + 2] << 8 | offs[i + 3];
        volatile uint32_t *r = P2V(base + off);
        uint32_t addr = r[SSPP_SRC0_ADDR / 4], size = r[SSPP_SRC_IMG_SIZE / 4], stride = r[SSPP_SRC_YSTRIDE0 / 4] & 0xffff;
        uint32_t fmt = r[SSPP_SRC_FORMAT / 4];
        if (!size) size = r[SSPP_SRC_SIZE / 4];
        uint32_t w = size & 0xffff, h = size >> 16, bypp = ((fmt >> 9) & 3) + 1;
        if (!addr || !w || !h || stride < w * bypp || (bypp != 3 && bypp != 4)) continue;
        platform.fb_base = addr; platform.fb_width = w; platform.fb_height = h;
        platform.fb_pitch = stride; platform.fb_bpp = bypp * 8;
        /* a tall phone panel has its front camera cut out of the top:
         * keep the console (and zwm's docks) below it */
        if (h > 2 * w) fb_safe_top = h / 20;
        return;
    }
}

/* A simple-framebuffer node (what a mainline-style tree says), or the splash. */
static void probe_framebuffer(void)
{
    int fb = fdt_find_compatible("simple-framebuffer");
    if (fb >= 0) {
        uint64_t b, sz;
        const char *fmt = fdt_prop(fb, "format", NULL);
        if (fdt_reg(fb, 0, &b, &sz) == 0 && fmt) {
            platform.fb_base = b;
            platform.fb_width = fdt_prop_u32(fb, "width", 0, 0);
            platform.fb_height = fdt_prop_u32(fb, "height", 0, 0);
            platform.fb_pitch = fdt_prop_u32(fb, "stride", 0, 0);
            platform.fb_bpp = strcmp(fmt, "r8g8b8") == 0 ? 24 : 32;
            if (platform.fb_width && platform.fb_height && platform.fb_pitch) return;
        }
        platform.fb_base = 0;
    }
    probe_qcom_splash();
}

static void scan_tree(void)
{
    int root = fdt_path("/");
    int len;
    const char *model = fdt_prop(root, "model", &len);
    if (model) { size_t n = strlen(model); if (n > sizeof platform.model - 1) n = sizeof platform.model - 1; memcpy(platform.model, model, n); }

    scan_memory(root);

    int chosen = fdt_path("/chosen");
    const uint8_t *p = fdt_prop(chosen, "linux,initrd-start", &len);
    if (p) {
        platform.initrd_base = len == 8 ? fdt_prop_u64(chosen, "linux,initrd-start", 0, 0) : fdt_prop_u32(chosen, "linux,initrd-start", 0, 0);
        uint64_t end = 0;
        p = fdt_prop(chosen, "linux,initrd-end", &len);
        if (p) end = len == 8 ? fdt_prop_u64(chosen, "linux,initrd-end", 0, 0) : fdt_prop_u32(chosen, "linux,initrd-end", 0, 0);
        platform.initrd_size = end > platform.initrd_base ? end - platform.initrd_base : 0;
        find_tar_in_initrd();
    }

    int uart = fdt_find_compatible("arm,pl011");            /* none on a phone: no console there but the screen */
    if (uart >= 0) {
        fdt_reg(uart, 0, &platform.uart_base, NULL);
        platform.uart_irq = (int)(32 + fdt_prop_u32(uart, "interrupts", 1, 1));
    }

    int gic = fdt_find_compatible("arm,gic-v3");
    if (gic >= 0) {
        platform.gic_version = 3;
        fdt_reg(gic, 0, &platform.gicd_base, NULL);
        fdt_reg(gic, 1, &platform.gicr_base, &platform.gicr_size);
    } else {
        gic = fdt_find_compatible("arm,cortex-a15-gic");
        if (gic < 0) gic = fdt_find_compatible("arm,gic-400");
        platform.gic_version = 2;
        platform.gicd_base = 0x08000000; platform.gicc_base = 0x08010000;
        fdt_reg(gic, 0, &platform.gicd_base, NULL);
        fdt_reg(gic, 1, &platform.gicc_base, NULL);
    }

    int wdt = fdt_find_compatible("qcom,msm-watchdog");
    if (wdt >= 0) fdt_reg(wdt, 0, &platform.wdt_base, NULL);

    const char *args = fdt_prop(chosen, "bootargs", NULL);
    if (args && strstr(args, "nosmp")) platform.nosmp = 1;

    platform.timer_freq = (uint32_t)read_sysreg(cntfrq_el0);
    int timer = fdt_find_compatible("arm,armv8-timer");
    if (timer >= 0) platform.timer_freq = fdt_prop_u32(timer, "clock-frequency", 0, platform.timer_freq);
    /* Its interrupts: secure, non-secure, virtual, hypervisor; ours is the
     * virtual one, a PPI whose number the board chooses. */
    platform.timer_irq = 27;
    if (timer >= 0 && gic >= 0) {
        uint32_t cells = fdt_prop_u32(gic, "#interrupt-cells", 0, 3);
        int len;
        if (fdt_prop(timer, "interrupts", &len) && len >= (int)(3 * cells * 4)) {
            uint32_t type = fdt_prop_u32(timer, "interrupts", (int)(2 * cells), 1), num = fdt_prop_u32(timer, "interrupts", (int)(2 * cells + 1), 11);
            platform.timer_irq = (int)((type == 1 ? 16 : 32) + num);
        }
    }

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
    if (platform.dtb_ok) { platform.dtb_size = (uint32_t)fdt_size(); scan_tree(); probe_framebuffer(); }
    else {
        platform.mem_base = 0x40000000; platform.mem_size = 128u << 20;
        platform.mem[0].base = platform.mem_base; platform.mem[0].size = platform.mem_size; platform.nmem = 1;
        platform.gic_version = 2;
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
    /* No tar from the loader: the one built into the image, if any. */
    extern char embedded_initrd_start[], embedded_initrd_end[];
    uintptr_t emb_s = (uintptr_t)embedded_initrd_start, emb_e = (uintptr_t)embedded_initrd_end;
    __asm__("" : "+r"(emb_s), "+r"(emb_e));     /* two symbols, not one array: compare the addresses */
    if (initrd_tar_at < 0 && emb_e > emb_s) {
        platform.initrd_base = V2P(embedded_initrd_start);
        platform.initrd_size = emb_e - emb_s;
        initrd_tar_at = -2;
    }
    boot_info.initrd_addr = platform.initrd_base;
    boot_info.initrd_size = platform.initrd_size;
    /* Usable RAM: the memory ranges the direct map reaches, minus firmware's
     * regions and what is already in use: the kernel, the initrd, the
     * device tree, the splash screen. */
    static struct iv usable[MMAP_MAX];
    int nu = 0;
    for (int i = 0; i < platform.nmem && nu < MMAP_MAX; i++) {
        uint64_t s0 = platform.mem[i].base, e0 = platform.mem[i].base + platform.mem[i].size;
        if (e0 > HHDM_SIZE) e0 = HHDM_SIZE;
        if (s0 < e0) { usable[nu].s = s0; usable[nu].e = e0; nu++; }
    }
    for (int i = 0; i < platform.nrsv; i++)
        nu = iv_subtract(usable, nu, MMAP_MAX, platform.rsv[i].base, platform.rsv[i].base + platform.rsv[i].size);
    uint64_t ini_s = platform.initrd_base & ~0xFFFULL, ini_e = (platform.initrd_base + platform.initrd_size + 0xFFF) & ~0xFFFULL;
    uint64_t dtb_s = dtb_phys & ~0xFFFULL, dtb_e = (dtb_phys + platform.dtb_size + 0xFFF) & ~0xFFFULL;
    nu = iv_subtract(usable, nu, MMAP_MAX, kphys, kend);
    int initrd_own = platform.initrd_size && initrd_tar_at != -2;     /* the built-in one is part of the kernel */
    if (initrd_own) nu = iv_subtract(usable, nu, MMAP_MAX, ini_s, ini_e);
    if (platform.dtb_ok) nu = iv_subtract(usable, nu, MMAP_MAX, dtb_s, dtb_e);
    if (platform.fb_base) nu = iv_subtract(usable, nu, MMAP_MAX, platform.fb_base & ~0xFFFULL,
                                           (platform.fb_base + (uint64_t)platform.fb_pitch * platform.fb_height + 0xFFF) & ~0xFFFULL);
    for (int i = 0; i < nu; i++) add_mmap(usable[i].s, usable[i].e - usable[i].s, ZAEBOOT_MEM_USABLE);
    add_mmap(kphys, kend - kphys, ZAEBOOT_MEM_BOOTLOADER);
    if (initrd_own) add_mmap(ini_s, ini_e - ini_s, ZAEBOOT_MEM_BOOTLOADER);
    if (platform.dtb_ok) add_mmap(dtb_s, dtb_e - dtb_s, ZAEBOOT_MEM_RESERVED);
    for (uint32_t i = 1; i < boot_info.mmap_count; i++)
        for (uint32_t j = i; j > 0 && boot_mmap[j].base < boot_mmap[j - 1].base; j--) {
            struct zaeboot_mmap_entry t = boot_mmap[j]; boot_mmap[j] = boot_mmap[j - 1]; boot_mmap[j - 1] = t;
        }
    boot_info.mmap = (uint64_t)(uintptr_t)boot_mmap;

    /* A watchdog the bootloader left counting would reset us in seconds:
     * nothing here pets it. */
    if (platform.wdt_base && platform.wdt_base < 0x40000000)
        *(volatile uint32_t *)P2V(platform.wdt_base + 0x08) = 0;        /* WDT_EN */

    build_direct_map();
    if (platform.fb_base) {
        /* A sign of life on a board with no serial port: a green bar across
         * the top. The console clears it a moment later; if it stays, we
         * stopped in between. */
        volatile uint8_t *fb = P2V(platform.fb_base);
        uint32_t bypp = platform.fb_bpp / 8;
        for (uint32_t y = 0; y < 48 && y < platform.fb_height; y++)
            for (uint32_t x = 0; x < platform.fb_width; x++) {
                volatile uint8_t *px = fb + (uint64_t)y * platform.fb_pitch + x * bypp;
                px[0] = 0x40; px[1] = 0xd0; px[2] = 0x40;
                if (bypp == 4) px[3] = 0xff;
            }
        dsb(sy);
        boot_info.fb.base = platform.fb_base;
        boot_info.fb.width = platform.fb_width;
        boot_info.fb.height = platform.fb_height;
        boot_info.fb.pitch = platform.fb_pitch;
        boot_info.fb.bpp = platform.fb_bpp;
        boot_info.fb.red_shift = 16; boot_info.fb.green_shift = 8; boot_info.fb.blue_shift = 0;
    }

    cpus[0].self = &cpus[0];
    cpus[0].index = 0;
    cpus[0].mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffULL;
    cpu_count = 1;
    kernel_main(&boot_info);
    for (;;) ;
}
