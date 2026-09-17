/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Early boot on a PowerMac: read the device tree through OpenFirmware, build
 * the boot info the generic kernel expects, take the machine over from the
 * firmware and call kernel_main. */
#include "asm/of.h"
#include "asm/memlayout.h"
#include "asm/ppc_regs.h"
#include "asm/cpu.h"
#include "zaeboot.h"
#include "string.h"
#include "printf.h"

struct ppc_platform platform;

extern void kernel_main(struct zaeboot_info *info);
extern void ppc_install_vectors(void);
extern char _kernel_start[], _kernel_end[];

static struct zaeboot_info boot_info __attribute__((aligned(16)));
static struct zaeboot_mmap_entry boot_mmap[16];

/* Physical layout below the kernel image (loaded at 16 MiB):
 *   0x00000000 - 0x00003000  exception vectors
 *   0x00400000 - 0x00500000  hash page table (1 MiB, aligned) */
#define VECTORS_SIZE 0x3000
#define HTAB_PHYS    0x00400000
#define HTAB_SIZE    0x00100000

static uint32_t stdout_ih;

static void of_puts(const char *s) { if (stdout_ih) of_write(stdout_ih, s, (uint32_t)strlen(s)); }
static void of_puthex(uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    of_puts(b);
}

/* "reg"/"assigned-addresses" of a PCI child: the first address cell that is
 * a memory BAR (phys.hi bit 24/25). Returns the physical address. */
static uint32_t pci_node_mem_base(uint32_t ph)
{
    uint32_t cells[5 * 6];
    int n = of_getprop(ph, "assigned-addresses", cells, sizeof cells);
    if (n < 20) n = of_getprop(ph, "reg", cells, sizeof cells);
    for (int i = 0; i + 5 <= n / 4; i += 5) {
        uint32_t hi = cells[i];
        if (((hi >> 24) & 3) >= 2)          /* 32- or 64-bit memory space */
            return cells[i + 2];            /* phys.lo */
    }
    return 0;
}

static void scan_tree(void)
{
    uint32_t root = of_finddevice("/");
    uint32_t chosen = of_finddevice("/chosen");
    of_getprop(chosen, "stdout", &stdout_ih, 4);
    platform.stdout_ih = stdout_ih;

    /* RAM: /memory "reg" = (base, size) pairs; we use the range starting at 0. */
    uint32_t mem = of_find_by_type(root, "memory");
    uint32_t reg[16];
    int n = of_getprop(mem, "reg", reg, sizeof reg);
    for (int i = 0; i + 1 < n / 4; i += 2)
        if (reg[i] == 0) platform.mem_size = reg[i + 1];
    if (!platform.mem_size)
        platform.mem_size = 128u << 20;

    /* Who made the firmware: OpenBIOS means QEMU. It calls itself
     * "OpenFirmware 3" like Apple's, but only it has a /builtin node (its
     * console package lives there). */
    uint32_t openprom = of_finddevice("/openprom");
    char model[32] = { 0 };
    if (openprom) of_getprop(openprom, "model", model, sizeof model - 1);
    platform.is_qemu = strstr(model, "OpenBIOS") != NULL || strstr(model, "QEMU") != NULL ||
                       of_finddevice("/builtin") != 0;

    /* CPU clocks */
    uint32_t cpu = of_find_by_type(root, "cpu");
    platform.timebase_freq = of_getprop_u32(cpu, "timebase-frequency", 16666666);
    platform.cpu_freq = of_getprop_u32(cpu, "clock-frequency", 0);

    /* Framebuffer: the display node behind stdout, or the first display. */
    uint32_t disp = stdout_ih ? of_instance_to_package(stdout_ih) : 0;
    char type[32] = { 0 };
    if (disp) of_getprop(disp, "device_type", type, sizeof type - 1);
    if (!disp || strcmp(type, "display") != 0)
        disp = of_find_by_type(root, "display");
    if (disp) {
        platform.fb_base = of_getprop_u32(disp, "address", 0);
        platform.fb_width = of_getprop_u32(disp, "width", 0);
        platform.fb_height = of_getprop_u32(disp, "height", 0);
        platform.fb_pitch = of_getprop_u32(disp, "linebytes", 0);
        platform.fb_depth = of_getprop_u32(disp, "depth", 0);
    }

    /* PCI host bridges: the config-space register base is "reg" (uni-north:
     * f2000000 etc.); the I/O window is the first "ranges" entry with space
     * code 1. Up to three bridges on a mac99. */
    int nb = 0;
    for (uint32_t node = of_child(root); node && nb < 3; node = of_peer(node)) {
        char t[32] = { 0 };
        if (of_getprop(node, "device_type", t, sizeof t - 1) <= 0 || strcmp(t, "pci") != 0)
            continue;
        uint32_t r[4];
        if (of_getprop(node, "reg", r, sizeof r) >= 8)
            platform.pci_cfg_base[nb++] = r[0];
        /* Interrupt routing: the bridge's "interrupt-map" lists, per device
         * (bus/dev/func in phys.hi) and INTx pin, the OpenPIC source. A
         * device node's own "interrupts" is only the pin here. */
        static uint32_t imap[32 * 7];
        int im = of_getprop(node, "interrupt-map", imap, sizeof imap) / 4;
        for (int i = 0; i + 7 <= im && platform.pci_irq_count < 32; i += 7) {
            int k = platform.pci_irq_count++;
            platform.pci_irq[k].bus = (uint8_t)(imap[i] >> 16);
            platform.pci_irq[k].slot = (uint8_t)((imap[i] >> 11) & 31);
            platform.pci_irq[k].func = (uint8_t)((imap[i] >> 8) & 7);
            platform.pci_irq[k].irq = (int)imap[i + 5];
        }
        uint32_t ranges[6 * 4];
        int rn = of_getprop(node, "ranges", ranges, sizeof ranges) / 4;
        for (int i = 0; i + 6 <= rn; i += 6)
            if (((ranges[i] >> 24) & 3) == 1 && !platform.pci_io_base)
                platform.pci_io_base = ranges[i + 3];      /* parent address */
    }

    /* mac-io: the southbridge everything else hangs off; the OpenPIC is its child. */
    platform.escc_irq = -1;
    uint32_t macio = of_find_by_name(root, "mac-io");
    if (macio) {
        /* IDE: "ata-4" (Heathrow/KeyLargo cells) and friends, "reg" = (offset, size) */
        for (uint32_t n = of_child(macio); n && platform.ide_count < 4; n = of_peer(n)) {
            char nm[16] = { 0 }, dt[16] = { 0 };
            of_getprop(n, "name", nm, sizeof nm - 1);
            of_getprop(n, "device_type", dt, sizeof dt - 1);
            if (memcmp(nm, "ata", 3) != 0 && strcmp(nm, "ide") != 0 && strcmp(dt, "ata") != 0 && strcmp(dt, "ide") != 0)
                continue;
            uint32_t r[2];
            if (of_getprop(n, "reg", r, sizeof r) >= 4)
                platform.ide_offset[platform.ide_count++] = r[0];
        }
        /* escc/ch-a "interrupts" = (source, sense) on the OpenPIC */
        uint32_t escc = of_find_by_name(macio, "escc");
        uint32_t cha = escc ? of_find_by_name(escc, "ch-a") : 0;
        uint32_t ints[2];
        if (cha && of_getprop(cha, "interrupts", ints, sizeof ints) >= 4) {
            platform.escc_irq = (int)ints[0];
            platform.escc_irq_level = ints[1] & 1;
        }
        platform.macio_base = pci_node_mem_base(macio);
        uint32_t pic = of_find_by_type(macio, "open-pic");
        if (!pic) pic = of_find_by_name(macio, "interrupt-controller");
        if (pic) {
            uint32_t preg[2];
            if (of_getprop(pic, "reg", preg, sizeof preg) >= 4)
                platform.openpic_base = platform.macio_base + preg[0];
        }
    }
}

static void add_mmap(uint64_t base, uint64_t len, uint32_t type)
{
    if (boot_info.mmap_count >= 16 || len == 0) return;
    boot_mmap[boot_info.mmap_count].base = base;
    boot_mmap[boot_info.mmap_count].length = len;
    boot_mmap[boot_info.mmap_count].type = type;
    boot_info.mmap_count++;
}

void ppc_early_main(void *of, uint32_t initrd, uint32_t initrd_size)
{
    of_init(of);
    scan_tree();
    platform.initrd_base = initrd;
    platform.initrd_size = initrd_size;

    of_puts("\r\nsic/powerpc: taking over from OpenFirmware\r\n");
    of_puts("  ram "); of_puthex(platform.mem_size);
    of_puts("  timebase "); of_puthex(platform.timebase_freq);
    of_puts("  macio "); of_puthex(platform.macio_base);
    of_puts("  openpic "); of_puthex(platform.openpic_base);
    of_puts("  escc irq "); of_puthex((uint32_t)platform.escc_irq);
    of_puts("\r\n  pci cfg "); of_puthex(platform.pci_cfg_base[0]); of_puts(" "); of_puthex(platform.pci_cfg_base[1]);
    of_puts("  io "); of_puthex(platform.pci_io_base);
    of_puts("\r\n  fb "); of_puthex(platform.fb_base); of_puts(" "); of_puthex(platform.fb_width); of_puts("x");
    of_puthex(platform.fb_height); of_puts(" pitch "); of_puthex(platform.fb_pitch); of_puts(" depth "); of_puthex(platform.fb_depth);
    of_puts("\r\n  initrd "); of_puthex(initrd); of_puts(" + "); of_puthex(initrd_size);
    of_puts("\r\n");

    /* Boot info in the shape the generic kernel expects. Addresses are
     * virtual (direct map) where the kernel dereferences them. */
    memset(&boot_info, 0, sizeof boot_info);
    boot_info.magic = ZAEBOOT_MAGIC;
    boot_info.version = ZAEBOOT_VERSION;
    boot_info.size = sizeof boot_info;
    boot_info.firmware = ZAEBOOT_FW_OPENFIRMWARE;
    boot_info.fb.base = platform.fb_base;
    boot_info.fb.width = platform.fb_width;
    boot_info.fb.height = platform.fb_height;
    boot_info.fb.pitch = platform.fb_pitch;
    boot_info.fb.bpp = platform.fb_depth;
    boot_info.fb.red_shift = 16; boot_info.fb.green_shift = 8; boot_info.fb.blue_shift = 0;
    uint32_t kphys = (uint32_t)((uintptr_t)_kernel_start - HHDM_BASE);
    uint32_t kend = ((uint32_t)((uintptr_t)_kernel_end - HHDM_BASE) + 0xFFF) & ~0xFFFu;
    boot_info.kernel_phys_base = kphys;
    boot_info.kernel_size = kend - kphys;
    boot_info.initrd_addr = initrd;
    boot_info.initrd_size = initrd_size;
    add_mmap(0, VECTORS_SIZE, ZAEBOOT_MEM_RESERVED);
    add_mmap(VECTORS_SIZE, HTAB_PHYS - VECTORS_SIZE, ZAEBOOT_MEM_USABLE);
    add_mmap(HTAB_PHYS, HTAB_SIZE, ZAEBOOT_MEM_RESERVED);
    add_mmap(HTAB_PHYS + HTAB_SIZE, kphys - HTAB_PHYS - HTAB_SIZE, ZAEBOOT_MEM_USABLE);
    add_mmap(kphys, kend - kphys, ZAEBOOT_MEM_BOOTLOADER);
    uint32_t top = platform.mem_size - (4u << 20);      /* the firmware's own memory sits at the top */
    if (initrd && initrd_size) {
        uint32_t iend = (initrd + initrd_size + 0xFFF) & ~0xFFFu;
        add_mmap(kend, initrd - kend, ZAEBOOT_MEM_USABLE);
        add_mmap(initrd, iend - initrd, ZAEBOOT_MEM_BOOTLOADER);
        add_mmap(iend, top - iend, ZAEBOOT_MEM_USABLE);
    } else {
        add_mmap(kend, top - kend, ZAEBOOT_MEM_USABLE);
    }
    add_mmap(top, platform.mem_size - top, ZAEBOOT_MEM_RESERVED);
    boot_info.mmap = (uint64_t)(uintptr_t)boot_mmap;

    /* From here on the firmware is history: its translations and exception
     * handlers go, ours come. */
    of_quiesce();
    of_init(NULL);
    cpus[0].self = &cpus[0];
    cpus[0].index = 0;
    cpu_count = 1;
    __asm__ volatile("mtspr 275, %0" : : "r"(&cpus[0]));       /* SPRG3 = per-CPU block */
    ppc_install_vectors();
    kernel_main(&boot_info);
    for (;;) ;
}
