/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The buttons of a Qualcomm phone. They hang off the PMIC, reached over
 * SPMI through the PMIC arbiter: power and volume down are the power-on
 * block's KPDPWR and RESIN lines, volume up is one of the PMIC's GPIOs
 * (where the device tree's gpio-keys node says). Read-only, through the
 * arbiter's observer channels, polled every 20 ms. They become keyboard
 * scancodes: power is Enter (the Power key, E0 5E, while a program has the
 * keyboard raw: the window server turns the screen off with it), the
 * volume keys are the up and down arrows. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/timer.h"
#include "drivers/keyboard.h"
#include "abi/fb.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"

#define ARB_VERSION     0x0000
#define ARB_FEATURES    0x0004
#define ARB_APID_MAP(n) (0x900 + 4 * (n))
#define CH_CMD          0x00
#define CH_STATUS       0x08
#define CH_RDATA0       0x18
#define OP_EXT_READL    1

#define PON_INT_RT_STS  0x10            /* in the PON peripheral: bit 0 KPDPWR, bit 1 RESIN */
#define GPIO_STATUS1    0x08            /* in a PMIC GPIO: bit 0 the level */
#define GPIO_EN_CTL     0x46            /* bit 7: the pin is enabled */

static volatile uint8_t *core, *obs, *chnls, *cnfg;
static uint32_t apid_count;
static int ee;

/* The arbiter channel (APID) that serves a peripheral (PPID: SID and the
 * high byte of the address). */
static int find_apid(uint16_t ppid)
{
    for (uint32_t n = 0; n < apid_count && ARB_APID_MAP(n) < 0x1100; n++) {
        uint32_t v = mmio_read32(core + ARB_APID_MAP(n));
        if (v && ((v >> 8) & 0xfff) == ppid) return (int)n;
    }
    return -1;
}

/* The channel for writing to a peripheral: v5 maps a peripheral to
 * several, one per owner; ours is the one whose owner is this EE. */
static int find_apid_owned(uint16_t ppid)
{
    if (!cnfg) return -1;
    for (uint32_t n = 0; n < apid_count && ARB_APID_MAP(n) < 0x1100; n++) {
        uint32_t v = mmio_read32(core + ARB_APID_MAP(n));
        if (v && ((v >> 8) & 0xfff) == ppid && (mmio_read32(cnfg + 0x700 + 4 * n) & 7) == (uint32_t)ee) return (int)n;
    }
    return -1;
}

static int spmi_read(int apid, uint16_t addr, uint8_t *out)
{
    volatile uint8_t *ch = obs + 0x10000 * (uint32_t)ee + 0x80 * (uint32_t)apid;
    mmio_write32(ch + CH_CMD, (uint32_t)OP_EXT_READL << 27 | (uint32_t)(addr & 0xff) << 4);   /* one byte */
    for (int i = 0; i < 100000; i++) {
        uint32_t st = mmio_read32(ch + CH_STATUS);
        if (st & 1) {
            if (st & 0xe) return -1;    /* failure, denied, dropped */
            *out = (uint8_t)mmio_read32(ch + CH_RDATA0);
            return 0;
        }
    }
    return -1;
}

/* A byte of a PMIC register (SID, 16-bit address), for other drivers
 * (the WLAN's regulators); -1 if no channel serves it or the read fails. */
int qcom_pmic_read(unsigned sid, uint16_t addr, uint8_t *out)
{
    if (!core || !obs) return -1;
    int apid = find_apid((uint16_t)(sid << 8 | addr >> 8));
    return apid < 0 ? -1 : spmi_read(apid, addr, out);
}

/* A write, through the channel's owner registers: only if this EE owns it. */
static int spmi_write(int apid, uint16_t addr, uint8_t v)
{
    if (!chnls || !cnfg || (mmio_read32(cnfg + 0x700 + 4 * (uint32_t)apid) & 7) != (uint32_t)ee) return -1;
    volatile uint8_t *ch = chnls + 0x10000 * (uint32_t)apid;
    mmio_write32(ch + 0x10, v);                                          /* WDATA0 */
    mmio_write32(ch + CH_CMD, (uint32_t)(addr & 0xff) << 4);             /* EXT_WRITEL (0), one byte */
    for (int i = 0; i < 100000; i++) {
        uint32_t st = mmio_read32(ch + CH_STATUS);
        if (st & 1) return st & 0xe ? -1 : 0;
    }
    return -1;
}

static int pon_apid = -1, volup_apid = -1;
static uint16_t pon_base, volup_base;
static uint8_t sid;

/* Scancode set 1: Enter or Power, and the extended up/down arrows. */
static void press(int key, int down)
{
    switch (key) {
    case 0:                                                                                 /* power */
        if (keyboard_get_mode() == K_RAW) { keyboard_scancode(0xe0); keyboard_scancode(down ? 0x5e : 0xde); }
        else keyboard_scancode(down ? 0x1c : 0x9c);                                         /* Enter, for text menus */
        break;
    case 1: keyboard_scancode(0xe0); keyboard_scancode(down ? 0x48 : 0xc8); break;          /* volume up: Up */
    case 2: keyboard_scancode(0xe0); keyboard_scancode(down ? 0x50 : 0xd0); break;          /* volume down: Down */
    }
}

/* Both volume keys held: the screen back to the console, and on it (and
 * the USB console) every task with where it waits, the heap and the GPU.
 * The task dump comes from an idle loop: if it never shows, the CPUs are
 * stuck themselves. */
static void hang_report(void)
{
    extern void fb_force_text(void);
    extern void sched_dump_request(void);
    extern void qcom_gpu_dump(void);
    fb_force_text();
    kprintf("=== volume up + down: hang report at %llu ms ===\n", timer_ms());
    qcom_gpu_dump();
    sched_dump_request();
}

static int sample(int key)
{
    uint8_t v;
    if (key == 1) return volup_apid >= 0 && spmi_read(volup_apid, volup_base + GPIO_STATUS1, &v) == 0 && !(v & 1);   /* active low */
    if (pon_apid < 0 || spmi_read(pon_apid, pon_base + PON_INT_RT_STS, &v) != 0) return 0;
    return key == 0 ? (v & 1) : (v >> 1) & 1;
}

static void keys_thread(void *arg)
{
    (void)arg;
    int state[3] = { 0, 0, 0 }, last[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) state[k] = last[k] = sample(k);   /* held at boot: not a press */
    for (;;) {
        task_sleep_ms(20);
        for (int k = 0; k < 3; k++) {
            int now = sample(k);
            if (now == last[k] && now != state[k]) {            /* two samples agree: debounced */
                state[k] = now;
                press(k, now);
            }
            last[k] = now;
        }
        static int combo;                                       /* both volume keys: what is everyone doing? */
        if (state[1] && state[2] && !combo) { combo = 1; hang_report(); }
        if (!state[1] && !state[2]) combo = 0;
    }
}

/* The reason the bootloader reads after a restart (1: stay in fastboot, 0:
 * none), in the power-on block's SOFT_RB_SPARE, bits 7:1. */
int qcom_pon_set_reason(uint8_t reason)
{
    uint8_t v = 0, after = 0;
    if (pon_apid < 0 || spmi_read(pon_apid, pon_base + 0x8f, &v) != 0) { kprintf("pon: cannot read SOFT_RB_SPARE\n"); return -1; }
    int wapid = find_apid_owned((uint16_t)(sid << 8 | pon_base >> 8));
    int rc = wapid >= 0 ? spmi_write(wapid, pon_base + 0x8f, (uint8_t)((v & 1) | reason << 1)) : -1;
    spmi_read(pon_apid, pon_base + 0x8f, &after);
    kprintf("pon: SOFT_RB_SPARE %02x -> %02x (write %d through channel %d)\n", v, after, rc, wapid);
    return rc;
}

void qcom_keys_init(void)
{
    int arb = fdt_find_compatible("qcom,spmi-pmic-arb");
    uint64_t core_phys, obs_phys;
    if (arb < 0 || fdt_reg(arb, 0, &core_phys, NULL) != 0 || fdt_reg(arb, 2, &obs_phys, NULL) != 0) return;
    if (core_phys >= 0x40000000 || obs_phys >= 0x40000000) return;
    core = P2V(core_phys);
    obs = P2V(obs_phys);
    uint64_t chnls_phys, cnfg_phys;
    if (fdt_reg(arb, 1, &chnls_phys, NULL) == 0 && chnls_phys < 0x40000000) chnls = P2V(chnls_phys);
    if (fdt_reg(arb, 4, &cnfg_phys, NULL) == 0 && cnfg_phys < 0x40000000) cnfg = P2V(cnfg_phys);
    ee = (int)fdt_prop_u32(arb, "qcom,ee", 0, 0);
    uint32_t ver = mmio_read32(core + ARB_VERSION);
    if (ver < 0x50000000 || ver >= 0x70000000) { kprintf("keys: PMIC arbiter version %x: only v5 is known\n", ver); return; }
    apid_count = mmio_read32(core + ARB_FEATURES) & 0x7ff;

    /* power and volume down: the PMIC's power-on block */
    int pon = fdt_find_compatible("qcom,qpnp-power-on");
    if (pon < 0) pon = fdt_find_compatible("qcom,pm8941-pon");
    int pmic = pon >= 0 ? fdt_parent(pon) : -1;
    uint64_t v;
    if (pmic >= 0 && fdt_reg(pmic, 0, &v, NULL) == 0) sid = (uint8_t)v;
    if (pon >= 0 && fdt_reg(pon, 0, &v, NULL) == 0) {
        pon_base = (uint16_t)v;
        pon_apid = find_apid((uint16_t)(sid << 8 | pon_base >> 8));
    }
    /* volume up: the gpio-keys entry, a PMIC GPIO (the controller's base + 0x100 per pin) */
    int gk = fdt_find_compatible("gpio-keys");
    for (int c = gk >= 0 ? fdt_first_child(gk) : -1; c >= 0; c = fdt_next_sibling(c)) {
        if (fdt_prop_u32(c, "linux,code", 0, 0) != 115) continue;          /* KEY_VOLUMEUP */
        uint32_t ph = fdt_prop_u32(c, "gpios", 0, 0), pin = fdt_prop_u32(c, "gpios", 1, 0);
        for (int n = fdt_path("/"); n >= 0; n = fdt_walk_next(n))
            if (fdt_prop_u32(n, "phandle", 0, 0) == ph && fdt_reg(n, 0, &v, NULL) == 0) {
                volup_base = (uint16_t)(v + (pin - 1) * 0x100);
                volup_apid = find_apid((uint16_t)(sid << 8 | volup_base >> 8));
                break;
            }
    }
    uint8_t en = 0;
    if (volup_apid >= 0) spmi_read(volup_apid, volup_base + GPIO_EN_CTL, &en);
    kprintf("keys: PMIC arbiter v%x, %u channels; power-on block %x (channel %d), volume-up GPIO %x (channel %d%s)\n",
            ver >> 28, apid_count, pon_base, pon_apid, volup_base, volup_apid, volup_apid >= 0 && !(en & 0x80) ? ", disabled" : "");
    if (volup_apid >= 0 && !(en & 0x80)) volup_apid = -1;                  /* a disabled pin reads as pressed */
    if (pon_apid >= 0 || volup_apid >= 0) task_create("keys", keys_thread, NULL);
}
