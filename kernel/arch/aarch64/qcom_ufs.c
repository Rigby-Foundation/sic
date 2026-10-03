/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A Qualcomm phone's UFS flash: the controller the bootloader left
 * running, handed to drivers/ufs.c once its power domain and clocks are
 * seen on (a register read of an unclocked block hangs the bus). The
 * clock controller's layout is KHAJE's (as Linux's gcc-sm6115 for BENGAL). */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/timer.h"
#include "drivers/ufs.h"
#include "endian.h"
#include "printf.h"

#define GCC_UFS_GDSCR       0x45004     /* bit 31 on, bit 0 collapse */
#define CBCR_OFF            (1u << 31)

static const struct { const char *name; uint32_t cbcr; int needed; } clocks[] = {
    { "ahb", 0x45014, 1 }, { "axi", 0x45010, 1 }, { "unipro", 0x45040, 1 }, { "ice", 0x45044, 0 },
    { "phy aux", 0x45078, 0 }, { "tx symbol", 0x45018, 0 }, { "rx symbol", 0x4501c, 0 },
};

static int clock_on(volatile uint8_t *gcc, uint32_t cbcr)
{
    if (!(mmio_read32(gcc + cbcr) & CBCR_OFF)) return 1;
    mmio_write32(gcc + cbcr, mmio_read32(gcc + cbcr) | 1);            /* CLK_ENABLE */
    uint64_t t0 = timer_ms();
    while (mmio_read32(gcc + cbcr) & CBCR_OFF)
        if (timer_ms() - t0 > 20) return 0;
    return 1;
}

void qcom_ufs_init(void)
{
    int n = fdt_find_compatible("qcom,ufshc"), gccn = fdt_find_compatible("qcom,khaje-gcc");
    uint64_t base, gcc_phys;
    if (n < 0 || gccn < 0 || fdt_reg(n, 0, &base, NULL) || fdt_reg(gccn, 0, &gcc_phys, NULL)) return;
    volatile uint8_t *gcc = P2V(gcc_phys);
    uint32_t gdsc = mmio_read32(gcc + GCC_UFS_GDSCR);
    if (!(gdsc & (1u << 31))) {
        mmio_write32(gcc + GCC_UFS_GDSCR, gdsc & ~1u);
        uint64_t t0 = timer_ms();
        while (!(mmio_read32(gcc + GCC_UFS_GDSCR) & (1u << 31)) && timer_ms() - t0 < 100) ;
    }
    if (!(mmio_read32(gcc + GCC_UFS_GDSCR) & (1u << 31))) { kprintf("ufs: its power domain is off (GDSCR %08x)\n", gdsc); return; }
    for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++)
        if (!clock_on(gcc, clocks[i].cbcr) && clocks[i].needed) {
            kprintf("ufs: the %s clock does not run (CBCR %08x): leaving it alone\n", clocks[i].name, mmio_read32(gcc + clocks[i].cbcr));
            return;
        }
    ufs_attach(P2V(base), 1);
}
