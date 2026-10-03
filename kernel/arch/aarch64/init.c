/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* aarch64 boot-time initialisation, called from the generic kernel_main. */
#include "asm/arch.h"
#include "asm/cpu.h"
#include "asm/timer.h"
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/sysreg.h"
#include "asm/smp.h"
#include "string.h"
#include "printf.h"

struct cpu cpus[MAX_CPUS];
uint32_t cpu_count;

extern void gic_init(void);

void cpu_set_kernel_stack(uint64_t sp)
{
    this_cpu()->kernel_sp = sp;
}

void arch_cpu_init_boot(void)
{
    /* The vectors, the per-CPU block and the MMU were set up in head.S. */
    kprintf("device tree: %s at %llx (%u bytes)%s: ram %llx+%llx in %d range(s), %d reserved, uart %llx irq %d, gic v%d %llx/%llx, ecam %llx, pci mem %llx+%llx io %llx+%llx, %d irq map entries\n",
            platform.dtb_ok ? platform.model : "unreadable", platform.dtb_phys, platform.dtb_size, platform.dtb_ok ? "" : " (defaults)",
            platform.mem_base, platform.mem_size, platform.nmem, platform.nrsv, platform.uart_base, platform.uart_irq, platform.gic_version,
            platform.gicd_base, platform.gic_version == 3 ? platform.gicr_base : platform.gicc_base,
            platform.ecam_base, platform.pci_mem_base, platform.pci_mem_size, platform.pci_io_base, platform.pci_io_size, platform.pci_irq_count);
    if (platform.fb_base)
        kprintf("display: the bootloader's %ux%u %u-bit screen at %llx\n", platform.fb_width, platform.fb_height, platform.fb_bpp, platform.fb_base);
    if (platform.wdt_base) kprintf("watchdog at %llx: stopped\n", platform.wdt_base);
    extern uint64_t initrd_given_base, initrd_given_size;
    extern int64_t initrd_tar_at, initrd_ustar_at;
    if (initrd_given_size) {
        const uint8_t *b = P2V(initrd_given_base);
        kprintf("initrd: %llx+%llx from the loader, starts %02x %02x %02x %02x %02x %02x %02x %02x; tar at %lld, \"ustar\" first at %lld\n",
                initrd_given_base, initrd_given_size, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                (long long)initrd_tar_at, (long long)initrd_ustar_at);
    } else {
        kprintf("initrd: none in /chosen\n");
    }
    if (initrd_tar_at == -2) kprintf("initrd: using the one built into the kernel (%llu KiB)\n", platform.initrd_size >> 10);
}

void arch_init_interrupts(const struct zaeboot_info *info)
{
    (void)info;
    gic_init();
    timer_init();
}

void arch_reboot(int power_off);

/* A restart with a mode: on a Qualcomm phone "bootloader" stops in
 * fastboot, "recovery" boots recovery (the IMEM restart reason, and the
 * PMIC's for newer bootloaders). */
int arch_reboot_reason(const char *mode)
{
    int imem = fdt_find_compatible("qcom,msm-imem-restart_reason");
    uint64_t at, base;
    int parent = imem >= 0 ? fdt_parent(imem) : -1;
    uint32_t imem_v = 0; uint8_t pon_v = 0;
    if (mode && strcmp(mode, "bootloader") == 0) { imem_v = 0x77665500; pon_v = 2; }   /* reboot-mode: bootloader 2, recovery 1 */
    else if (mode && strcmp(mode, "recovery") == 0) { imem_v = 0x77665502; pon_v = 1; }
    if (!imem_v) return -1;
    if (imem >= 0 && parent >= 0 && fdt_reg(imem, 0, &at, NULL) == 0 && fdt_reg(parent, 0, &base, NULL) == 0) {
        *(volatile uint32_t *)P2V(base + at) = imem_v;
        kprintf("restart reason: imem %llx = %08x\n", base + at, *(volatile uint32_t *)P2V(base + at));
    }
    extern int qcom_pon_set_reason(uint8_t reason);
    int rc = qcom_pon_set_reason(pon_v);
    __asm__ volatile("dsb sy" ::: "memory");
    return rc;
}

void arch_reboot_mode(const char *mode)
{
    arch_reboot_reason(mode);
    arch_reboot(0);
}

/* PSCI SYSTEM_RESET / SYSTEM_OFF, through whichever conduit the tree names. */
void arch_reboot(int power_off)
{
    int psci = fdt_path("/psci");
    const char *method = psci >= 0 ? fdt_prop(psci, "method", NULL) : NULL;
    register uint64_t x0 __asm__("x0") = power_off ? 0x84000008u : 0x84000009u;
    if (method && strcmp(method, "hvc") == 0) __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    else if (psci >= 0) __asm__ volatile("smc #0" : "+r"(x0) : : "memory");
    arch_halt_forever();
}

int arch_skip_selftest(void)
{
    int chosen = fdt_path("/chosen");
    const char *args = chosen >= 0 ? fdt_prop(chosen, "bootargs", NULL) : NULL;
    return args && strstr(args, "noselftest") != NULL;
}

void arch_init_smp(void)
{
    extern void qcom_usb_probe(void);
    qcom_usb_probe();               /* before the other CPUs: its report survives a failed bring-up */
#ifdef CONFIG_SMP
    smp_init();
#endif
    extern void qcom_keys_init(void);
    qcom_keys_init();               /* a Qualcomm phone's buttons, if this is one */
    extern void physmem_init(void);
    physmem_init();
    extern void qcom_touch_init(void);
    qcom_touch_init();              /* a "creek" phone's touch controller, if this is one */
    extern void qcom_gpu_init(void);
    qcom_gpu_init();                /* its Adreno 610: /dev/adreno */
    extern void qcom_panel_init(void);
    qcom_panel_init();              /* its panel through the running DSI controller: /dev/panel */
    extern void qcom_i2c_init(void);
    qcom_i2c_init();                /* its I2C buses and the battery's fuel gauge: /dev/battery */
#ifdef CONFIG_UFS
    extern void qcom_ufs_init(void);
    qcom_ufs_init();                /* its flash, the bootloader's UFS controller: /dev/sdX */
#endif
}

void arch_halt_forever(void)
{
    for (;;) {
        interrupts_disable();
        __asm__ volatile("wfi");
    }
}

void arch_hypervisor_id(char out[13])
{
    /* No CPUID; the device tree's model string is the closest thing. */
    memset(out, 0, 13);
    if (strstr(platform.model, "QEMU") || strstr(platform.model, "virt"))
        memcpy(out, "QEMU virt", 9);
}
