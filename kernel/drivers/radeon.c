/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * ATI Radeon Xpress 200M/1100/1150 (RS480/RS482: 0x5954, 0x5955, 0x5974,
 * 0x5975), the integrated graphics of AMD laptops like the Dell Vostro
 * 1000: a framebuffer on the laptop panel when the boot loader could not
 * set a VBE mode.
 *
 * Follows Linux's radeonfb (drivers/video/fbdev/aty) for this family:
 * the panel's native timings and the PLL limits come from the video BIOS
 * (its shadow at 0xC0000), the mode is the panel's own resolution at
 * 32 bpp on CRTC1, with no scaler, through the LVDS output the BIOS
 * already powered. Probing only reads and reports; `echo on >
 * /proc/radeon` switches (a reboot brings back text mode if the panel
 * stays dark), `cat /proc/radeon` shows the report again.
 */
#include "drivers/pci.h"
#include "drivers/fb.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/sched.h"
#include "printf.h"
#include "string.h"
#include "zaeboot.h"
#include "abi/abi.h"
#include "mm/heap.h"
#include "mm/pmm.h"

/* registers (offsets into the 64 KiB MMIO aperture, BAR 2) */
#define CLOCK_CNTL_INDEX        0x0008
#define CLOCK_CNTL_DATA         0x000C
#define GEN_INT_CNTL            0x0040
#define CRTC_GEN_CNTL           0x0050
#define CRTC_EXT_CNTL           0x0054
#define DAC_CNTL                0x0058
#define I2C_CNTL_1              0x0094
#define MC_FB_LOCATION          0x0148
#define NB_TOM                  0x015C
#define MPP_TB_CONFIG           0x01C0
#define CRTC_H_TOTAL_DISP       0x0200
#define CRTC_H_SYNC_STRT_WID    0x0204
#define CRTC_V_TOTAL_DISP       0x0208
#define CRTC_V_SYNC_STRT_WID    0x020C
#define CRTC_OFFSET             0x0224
#define CRTC_OFFSET_CNTL        0x0228
#define CRTC_PITCH              0x022C
#define OVR_CLR                 0x0230
#define OVR_WID_LEFT_RIGHT      0x0234
#define OVR_WID_TOP_BOTTOM      0x0238
#define DISPLAY_BASE_ADDR       0x023C
#define FP_CRTC_H_TOTAL_DISP    0x0250
#define FP_CRTC_V_TOTAL_DISP    0x0254
#define CRTC_MORE_CNTL          0x027C
#define FP_GEN_CNTL             0x0284
#define FP2_GEN_CNTL            0x0288
#define FP_HORZ_STRETCH         0x028C
#define FP_VERT_STRETCH         0x0290
#define TMDS_CRC                0x02A0
#define TMDS_TRANSMITTER_CNTL   0x02A4
#define FP_H_SYNC_STRT_WID      0x02C4
#define FP_V_SYNC_STRT_WID      0x02C8
#define LVDS_GEN_CNTL           0x02D0
#define LVDS_PLL_CNTL           0x02D4
#define CRTC2_GEN_CNTL          0x03F8
#define CRTC2_DISPLAY_BASE_ADDR 0x033C
#define GRPH2_BUFFER_CNTL       0x03F0
#define OV0_SCALE_CNTL          0x0420
#define OV0_BASE_ADDR           0x043C
#define SUBPIC_CNTL             0x0540
#define CAP0_TRIG_CNTL          0x0950
#define CAP1_TRIG_CNTL          0x09C0
#define SURFACE_CNTL            0x0B00
#define SURFACE0_LOWER_BOUND    0x0B04
#define SURFACE0_UPPER_BOUND    0x0B08
#define SURFACE0_INFO           0x0B0C
#define VIPH_CONTROL            0x0C40
#define RBBM_STATUS             0x0E40
#define CRTC_STATUS             0x005C
#define R300_CRTC_TILE_X0_Y0    0x0350
#define DISP_MERGE_CNTL         0x0D60
#define DISP_RGB_OFFSET_EN      (1u << 8)
#define R300_LVDS_SRC_SEL_MASK  (3u << 18)      /* in LVDS_PLL_CNTL: 0 CRTC1, 1 CRTC2, 2 the scaler */
#define R300_LVDS_SRC_SEL_RMX   (2u << 18)
#define FP_HORZ_VERT_ACTIVE     0x0278
#define DAC_CNTL2               0x007C
#define DAC2_PALETTE_ACC_CTL    (1u << 5)       /* palette access: 0 CRTC1, 1 CRTC2 */
#define PALETTE_INDEX           0x00B0          /* write index 7:0, read index 23:16 */
#define PALETTE_30_DATA         0x00B8          /* 10 bits each of red, green, blue */
#define HORZ_KEEP_BITS          (7u << 28 | 1u << 31)   /* FP_LOOP_STRETCH, AUTO_RATIO_INC */
#define VERT_KEEP_BITS          (0x71000000u | 1u << 31)

/* PLL registers (through CLOCK_CNTL_INDEX/DATA) */
#define PPLL_CNTL               0x02
#define PPLL_REF_DIV            0x03
#define PPLL_DIV_3              0x07
#define VCLK_ECP_CNTL           0x08
#define HTOTAL_CNTL             0x09

#define CRTC_EXT_DISP_EN        (1u << 24)
#define CRTC_EN                 (1u << 25)
#define CRTC_DBL_SCAN_EN        (1u << 0)
#define CRTC_INTERLACE_EN       (1u << 1)
#define VGA_ATI_LINEAR          (1u << 3)
#define XCRT_CNT_EN             (1u << 6)
#define CRTC_HSYNC_DIS          (1u << 8)
#define CRTC_VSYNC_DIS          (1u << 9)
#define CRTC_DISPLAY_DIS        (1u << 10)
#define DAC_MASK_ALL            (0xFFu << 24)
#define DAC_VGA_ADR_EN          (1u << 13)
#define DAC_8BIT_EN             (1u << 8)
#define DAC_RANGE_CNTL          (3u << 0)
#define DAC_BLANKING            (1u << 2)
#define HORZ_PANEL_SHIFT        16
#define VERT_PANEL_SHIFT        12
#define HORZ_AUTO_RATIO         (1u << 27)
#define VERT_AUTO_RATIO_EN      (1u << 27)
#define FP_FPON                 (1u << 0)
#define FP_TMDS_EN              (1u << 2)
#define FP_PANEL_FORMAT         (1u << 3)
#define FP_SEL_CRTC2            (1u << 13)
#define FP_CRTC_DONT_SHADOW_VPAR (1u << 16)
#define FP_CRTC_DONT_SHADOW_HEND (1u << 17)
#define FP_CRTC_USE_SHADOW_VEND (1u << 18)
#define FP_RMX_HVSYNC_CONTROL_EN (1u << 20)
#define FP_DFP_SYNC_SEL         (1u << 21)
#define FP_CRTC_LOCK_8DOT       (1u << 22)
#define FP_CRT_SYNC_SEL         (1u << 23)
#define FP_USE_SHADOW_EN        (1u << 24)
#define FP_CRT_SYNC_ALT         (1u << 26)
#define R200_FP_SOURCE_SEL_MASK (3u << 10)
#define R200_FP_SOURCE_SEL_CRTC1 (0u << 10)
#define LVDS_ON                 (1u << 0)
#define LVDS_BLON               (1u << 19)
#define PPLL_RESET              0x00000001u
#define PPLL_SLEEP              0x00000002u
#define PPLL_ATOMIC_UPDATE_EN   0x00010000u
#define PPLL_VGA_ATOMIC_UPDATE_EN 0x00020000u
#define PPLL_DIV_SEL_MASK       0x00000300u
#define PPLL_REF_DIV_MASK       0x000003FFu
#define R300_PPLL_REF_DIV_ACC_MASK (0x3FFu << 18)
#define R300_PPLL_REF_DIV_ACC_SHIFT 18
#define PPLL_FB3_DIV_MASK       0x000007FFu
#define PPLL_POST3_DIV_MASK     0x00070000u
#define PPLL_ATOMIC_UPDATE_R    0x00008000u
#define PPLL_ATOMIC_UPDATE_W    0x00008000u
#define VCLK_SRC_SEL_MASK       0x03u
#define VCLK_SRC_SEL_CPUCLK     0x00u
#define VCLK_SRC_SEL_PPLLCLK    0x03u
#define PLL_WR_EN               0x80u
#define DST_32BPP               6

static volatile uint8_t *mmio;
static const struct pci_dev *dev;
static uint64_t vram_phys, vram_size;
static const uint8_t *bios;             /* the video BIOS shadow, or NULL */
static uint16_t fp_bios_start;
static int on;

static struct {                         /* from the BIOS LCD table */
    int valid;
    char id[25];
    uint32_t xres, yres, hblank, hover, hsync, vblank, vover, vsync, clock;   /* clock: 10 kHz */
    uint32_t ref_div, post_div, fbk_div;
    int bios_dividers;
} panel;

static struct {
    uint32_t ref_clk, ref_div, ppll_min, ppll_max;   /* 10 kHz */
} pll = { 2700, 0, 12000, 35000 };

/* Everything the switch writes, as it was, to put back when nobody says
 * "keep" in time (a dark panel then comes back to text mode by itself). */
static const uint32_t saved_regs[] = {
    CRTC_GEN_CNTL, CRTC_EXT_CNTL, CRTC_MORE_CNTL, DAC_CNTL, CRTC_H_TOTAL_DISP, CRTC_H_SYNC_STRT_WID,
    CRTC_V_TOTAL_DISP, CRTC_V_SYNC_STRT_WID, CRTC_OFFSET, CRTC_OFFSET_CNTL, CRTC_PITCH, SURFACE_CNTL,
    FP_CRTC_H_TOTAL_DISP, FP_CRTC_V_TOTAL_DISP, FP_H_SYNC_STRT_WID, FP_V_SYNC_STRT_WID, FP_HORZ_STRETCH,
    FP_VERT_STRETCH, FP_HORZ_VERT_ACTIVE, DAC_CNTL2, FP_GEN_CNTL, LVDS_GEN_CNTL, LVDS_PLL_CNTL, DISP_MERGE_CNTL, R300_CRTC_TILE_X0_Y0,
    MC_FB_LOCATION, DISPLAY_BASE_ADDR, CRTC2_DISPLAY_BASE_ADDR, OV0_BASE_ADDR, GRPH2_BUFFER_CNTL,
    OVR_CLR, OVR_WID_LEFT_RIGHT, OVR_WID_TOP_BOTTOM, OV0_SCALE_CNTL, SUBPIC_CNTL, VIPH_CONTROL, I2C_CNTL_1,
    GEN_INT_CNTL, CAP0_TRIG_CNTL, CAP1_TRIG_CNTL,
};
#define NSAVED (sizeof saved_regs / sizeof saved_regs[0])
static uint32_t saved[NSAVED], saved_surf[24], saved_vclk, saved_pal[256];
static uint8_t *saved_vram;             /* the start of the memory: VGA text mode's font and characters */
static size_t saved_vram_len;
static volatile uint32_t *fbv;
static volatile int keep;

static char report[2048];
static size_t report_len;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static inline void wr8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(mmio + r) = v; }
static void wrp(uint32_t r, uint32_t v, uint32_t keep) { wr(r, (rd(r) & keep) | v); }

static uint32_t pll_rd(uint32_t idx)
{
    wr8(CLOCK_CNTL_INDEX, (uint8_t)(idx & 0x3F));
    return rd(CLOCK_CNTL_DATA);
}

static void pll_wr(uint32_t idx, uint32_t v)
{
    wr8(CLOCK_CNTL_INDEX, (uint8_t)((idx & 0x3F) | PLL_WR_EN));
    wr(CLOCK_CNTL_DATA, v);
}

static void pll_wrp(uint32_t idx, uint32_t v, uint32_t keep) { pll_wr(idx, (pll_rd(idx) & keep) | v); }

static void fifo_wait(int entries)
{
    for (int i = 0; i < 2000000; i++)
        if ((int)(rd(RBBM_STATUS) & 0x7F) >= entries)
            return;
    kprintf("radeon: FIFO timeout\n");
}

static uint16_t b16(uint32_t off) { return (uint16_t)(bios[off] | bios[off + 1] << 8); }
static uint32_t b32(uint32_t off) { return (uint32_t)b16(off) | (uint32_t)b16(off + 2) << 16; }

/* the report: printed at boot, and what /proc/radeon reads */
static void ap(const char *s) { while (*s && report_len < sizeof report - 1) report[report_len++] = *s++; }
static void apn(uint64_t v, int base, int width)
{
    char t[24];
    int n = 0;
    do { t[n++] = "0123456789abcdef"[v % (uint64_t)base]; v /= (uint64_t)base; } while (v);
    while (n < width) t[n++] = '0';
    while (n && report_len < sizeof report - 1) report[report_len++] = t[--n];
}
static void apu(uint64_t v) { apn(v, 10, 0); }
static void apx(uint64_t v) { apn(v, 16, 0); }
static void apmhz(uint32_t v10k) { apu(v10k / 100); ap("."); apn(v10k % 100, 10, 2); }

/* The IGP's video BIOS has a shadow copy in the first megabyte; the ATI
 * flat-panel tables hang off the word at 0x48. */
static void read_bios(void)
{
    const uint8_t *b = P2V(0xC0000);
    if (b[0] != 0x55 || b[1] != 0xAA) { ap("radeon: no video BIOS at 0xC0000\n"); return; }
    bios = b;
    fp_bios_start = b16(0x48);

    uint16_t pllb = b16(fp_bios_start + 0x30);
    if (pllb) {
        pll.ref_clk = b16(pllb + 0x0E);
        pll.ref_div = b16(pllb + 0x10);
        pll.ppll_min = b32(pllb + 0x12);
        pll.ppll_max = b32(pllb + 0x16);
    }
    uint16_t t = b16(fp_bios_start + 0x40);
    if (!t) { ap("radeon: the BIOS has no LCD table\n"); return; }
    for (int i = 0; i < 24; i++) { char c = (char)bios[t + 1 + i]; panel.id[i] = c >= 32 && c < 127 ? c : ' '; }
    panel.xres = b16(t + 25);
    panel.yres = b16(t + 27);
    panel.ref_div = b16(t + 46);
    panel.post_div = bios[t + 48];
    panel.fbk_div = b16(t + 49);
    panel.bios_dividers = panel.ref_div != 0 && panel.fbk_div > 3;
    for (int i = 0; i < 32; i++) {
        uint16_t m = b16(t + 64 + i * 2);
        if (!m) break;
        if (b16(m) != panel.xres || b16(m + 2) != panel.yres) continue;
        panel.hblank = (uint32_t)(b16(m + 17) - b16(m + 19)) * 8;
        panel.hover = ((uint32_t)(b16(m + 21) - b16(m + 19) - 1) * 8) & 0x7FFF;
        panel.hsync = (uint32_t)bios[m + 23] * 8;
        panel.vblank = (uint32_t)(b16(m + 24) - b16(m + 26));
        panel.vover = (uint32_t)((b16(m + 28) & 0x7FF) - b16(m + 26));
        panel.vsync = (uint32_t)(b16(m + 28) & 0xF800) >> 11;
        panel.clock = b16(m + 9);
        panel.valid = panel.xres && panel.yres && panel.clock;
        break;
    }
}

/* The PLL dividers for `freq` (10 kHz units): radeon_calc_pll_regs. */
static void calc_pll(uint32_t freq, uint32_t *ref_div, uint32_t *div3)
{
    static const struct { uint32_t div, bits; } post[] = {
        { 1, 0 }, { 2, 1 }, { 4, 2 }, { 8, 3 }, { 3, 4 }, { 16, 5 }, { 6, 6 }, { 12, 7 },
    };
    if (freq > pll.ppll_max) freq = pll.ppll_max;
    if (freq * 12 < pll.ppll_min) freq = pll.ppll_min / 12;
    int k = -1;
    for (int i = 0; i < 8; i++) {
        uint32_t out = post[i].div * freq;
        if (out >= pll.ppll_min && out <= pll.ppll_max) { k = i; break; }
    }
    if (k < 0) k = 0;
    uint32_t out = post[k].div * freq;
    uint32_t fb = (pll.ref_div * out + pll.ref_clk / 2) / pll.ref_clk;
    *ref_div = pll.ref_div;
    *div3 = fb | (post[k].bits << 16);
}

static void write_pll(uint32_t ref_div, uint32_t div3)
{
    fifo_wait(20);
    /* the mobility workaround: the same dividers as now, leave the PLL alone */
    if (ref_div == (pll_rd(PPLL_REF_DIV) & PPLL_REF_DIV_MASK) &&
        div3 == (pll_rd(PPLL_DIV_3) & (PPLL_POST3_DIV_MASK | PPLL_FB3_DIV_MASK))) {
        wrp(CLOCK_CNTL_INDEX, 0x300 & PPLL_DIV_SEL_MASK, ~PPLL_DIV_SEL_MASK);
        return;
    }
    pll_wrp(VCLK_ECP_CNTL, VCLK_SRC_SEL_CPUCLK, ~VCLK_SRC_SEL_MASK);
    pll_wrp(PPLL_CNTL, PPLL_RESET | PPLL_ATOMIC_UPDATE_EN | PPLL_VGA_ATOMIC_UPDATE_EN,
            ~(PPLL_RESET | PPLL_ATOMIC_UPDATE_EN | PPLL_VGA_ATOMIC_UPDATE_EN));
    wrp(CLOCK_CNTL_INDEX, 0x300 & PPLL_DIV_SEL_MASK, ~PPLL_DIV_SEL_MASK);
    /* R300-class: the reference divider goes in the accumulator field */
    if (ref_div & R300_PPLL_REF_DIV_ACC_MASK)
        pll_wrp(PPLL_REF_DIV, ref_div, 0);
    else
        pll_wrp(PPLL_REF_DIV, ref_div << R300_PPLL_REF_DIV_ACC_SHIFT, ~R300_PPLL_REF_DIV_ACC_MASK);
    pll_wrp(PPLL_DIV_3, div3, ~PPLL_FB3_DIV_MASK);
    pll_wrp(PPLL_DIV_3, div3, ~PPLL_POST3_DIV_MASK);
    for (int i = 0; i < 1000000 && (pll_rd(PPLL_REF_DIV) & PPLL_ATOMIC_UPDATE_R); i++)
        ;
    pll_wrp(PPLL_REF_DIV, PPLL_ATOMIC_UPDATE_W, ~PPLL_ATOMIC_UPDATE_W);
    for (int i = 0; i < 10000 && (pll_rd(PPLL_REF_DIV) & PPLL_ATOMIC_UPDATE_R); i++)
        ;
    pll_wr(HTOTAL_CNTL, 0);
    pll_wrp(PPLL_CNTL, 0, ~(PPLL_RESET | PPLL_SLEEP | PPLL_ATOMIC_UPDATE_EN | PPLL_VGA_ATOMIC_UPDATE_EN));
    task_sleep_ms(5);
    pll_wrp(VCLK_ECP_CNTL, VCLK_SRC_SEL_PPLLCLK, ~VCLK_SRC_SEL_MASK);
}

static void save_state(size_t vram_len);
static void report_regs(const char *when);
static void revert_thread(void *arg);

/* The panel's native mode, 32 bpp, on CRTC1: radeonfb_set_par and
 * radeon_write_mode for an LCD on an R300-class chip. */
static int set_mode(int at_boot)
{
    if (!panel.valid) { kprintf("radeon: no panel timings from the BIOS, not switching\n"); return -1; }
    uint32_t xres = panel.xres, yres = panel.yres;
    uint32_t pitch_bytes = (xres * 4 + 0x3F) & ~0x3Fu;
    if ((uint64_t)pitch_bytes * yres > vram_size) { kprintf("radeon: %ux%u does not fit in %llu KiB\n", xres, yres, vram_size >> 10); return -1; }

    uint32_t htotal = xres + panel.hblank, hss = xres + panel.hover, hse = hss + panel.hsync;
    uint32_t vtotal = yres + panel.vblank, vss = yres + panel.vover, vse = vss + panel.vsync;
    uint32_t hsync_wid = (hse - hss) / 8, vsync_wid = vse - vss;
    if (hsync_wid == 0) hsync_wid = 1; else if (hsync_wid > 0x3F) hsync_wid = 0x3F;
    if (vsync_wid == 0) vsync_wid = 1; else if (vsync_wid > 0x1F) vsync_wid = 0x1F;
    uint32_t hsync_start = hss - 8 + 5;           /* hsync_fudge_fp for 32 bpp */
    uint32_t hpol = 0, vpol = 0;                  /* the BIOS panels: active high */

    uint32_t ref_div, div3;
    if (panel.bios_dividers) { ref_div = panel.ref_div; div3 = panel.fbk_div | panel.post_div << 16; }
    else calc_pll(panel.clock, &ref_div, &div3);

    uint32_t crtc_more = rd(CRTC_MORE_CNTL) & 0xFFFFFFF0u;
    uint32_t vclk = pll_rd(VCLK_ECP_CNTL);
    uint32_t crtc_pitch = pitch_bytes / 32;       /* in units of 8 pixels */
    crtc_pitch |= crtc_pitch << 16;

    if (!at_boot) save_state((size_t)pitch_bytes * yres);
    report_regs("before");

    /* the frame buffer is the start of the IGP's memory, which the CPU sees at BAR 0 */
    uint32_t tom = rd(NB_TOM);
    fifo_wait(6);
    wr(MC_FB_LOCATION, tom);
    wr(DISPLAY_BASE_ADDR, (tom & 0xFFFF) << 16);
    wr(CRTC2_DISPLAY_BASE_ADDR, (tom & 0xFFFF) << 16);
    wr(OV0_BASE_ADDR, (tom & 0xFFFF) << 16);
    wr(GRPH2_BUFFER_CNTL, rd(GRPH2_BUFFER_CNTL) & ~0x7F0000u);

    /* black before it shows (what VGA mode kept there is saved first) */
    if (!fbv) fbv = vmm_map_wc(vram_phys, (size_t)pitch_bytes * yres);
    if (!fbv) { kprintf("radeon: cannot map the frame buffer\n"); return -1; }
    if (saved_vram) for (size_t i = 0; i < saved_vram_len / 4; i++) ((uint32_t *)saved_vram)[i] = fbv[i];
    for (size_t i = 0; i < (size_t)pitch_bytes / 4 * yres; i++) fbv[i] = 0;

    fifo_wait(31);
    static const uint32_t common[] = { OVR_CLR, OVR_WID_LEFT_RIGHT, OVR_WID_TOP_BOTTOM, OV0_SCALE_CNTL, SUBPIC_CNTL,
                                       VIPH_CONTROL, I2C_CNTL_1, GEN_INT_CNTL, CAP0_TRIG_CNTL, CAP1_TRIG_CNTL };
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++) wr(common[i], 0);
    for (int i = 0; i < 8; i++) {
        wr(SURFACE0_LOWER_BOUND + 0x10 * i, 0);
        wr(SURFACE0_UPPER_BOUND + 0x10 * i, 0x1F);
        wr(SURFACE0_INFO + 0x10 * i, 0);
    }
    /* as Linux's KMS for RS400/RS480: no RGB offset, no tiling offset, and
     * keep the cursor bits of CRTC_GEN_CNTL; rs4xx wants the CRTC enabled
     * while its timing is set */
    wr(DISP_MERGE_CNTL, rd(DISP_MERGE_CNTL) & ~DISP_RGB_OFFSET_EN);
    wr(R300_CRTC_TILE_X0_Y0, 0);
    wr(CRTC_GEN_CNTL, (rd(CRTC_GEN_CNTL) & 0x00718000u) | CRTC_EXT_DISP_EN | CRTC_EN | DST_32BPP << 8);
    wrp(CRTC_EXT_CNTL, VGA_ATI_LINEAR | XCRT_CNT_EN, ~(CRTC_HSYNC_DIS | CRTC_VSYNC_DIS | CRTC_DISPLAY_DIS));
    wr(CRTC_MORE_CNTL, crtc_more);
    wrp(DAC_CNTL, DAC_MASK_ALL | DAC_VGA_ADR_EN | DAC_8BIT_EN, DAC_RANGE_CNTL | DAC_BLANKING);
    wr(CRTC_H_TOTAL_DISP, (((htotal / 8) - 1) & 0x3FF) | ((xres / 8) - 1) << 16);
    wr(CRTC_H_SYNC_STRT_WID, (hsync_start & 0x1FFF) | hsync_wid << 16 | hpol << 23);
    wr(CRTC_V_TOTAL_DISP, ((vtotal - 1) & 0xFFFF) | (yres - 1) << 16);
    wr(CRTC_V_SYNC_STRT_WID, ((vss - 1) & 0xFFF) | vsync_wid << 16 | vpol << 23);
    wr(CRTC_OFFSET, 0);
    wr(CRTC_OFFSET_CNTL, 0);
    wr(CRTC_PITCH, crtc_pitch);
    wr(SURFACE_CNTL, 0);

    write_pll(ref_div, div3);

    /* At 32 bpp each channel still goes through CRTC1's palette, which
     * text mode left holding its 16 colours: load a straight ramp. */
    wr(DAC_CNTL2, rd(DAC_CNTL2) & ~DAC2_PALETTE_ACC_CTL);
    wr(PALETTE_INDEX, 0);
    for (uint32_t i = 0; i < 256; i++) wr(PALETTE_30_DATA, i << 22 | i << 12 | i << 2);

    fifo_wait(10);
    /* The panel stays behind the scaler, as the BIOS left it and as both
     * of Linux's drivers keep it (KMS: RMX_FULL, radeonfb never moves it);
     * at the native size the scaler just passes the CRTC's timing through,
     * so it gets the same timing and no stretch. FP_GEN_CNTL is left as the
     * BIOS set it, as KMS does for LVDS. */
    wr(FP_CRTC_H_TOTAL_DISP, (((htotal / 8) - 1) & 0x3FF) | ((xres / 8) - 1) << 16);
    wr(FP_CRTC_V_TOTAL_DISP, ((vtotal - 1) & 0xFFFF) | (yres - 1) << 16);
    wr(FP_H_SYNC_STRT_WID, ((hss - 8) & 0x1FFF) | hsync_wid << 16 | hpol << 23);
    wr(FP_V_SYNC_STRT_WID, ((vss - 1) & 0xFFF) | vsync_wid << 16 | vpol << 23);
    wr(FP_HORZ_VERT_ACTIVE, 0);
    wr(FP_HORZ_STRETCH, (rd(FP_HORZ_STRETCH) & HORZ_KEEP_BITS) | ((xres / 8) - 1) << HORZ_PANEL_SHIFT);
    wr(FP_VERT_STRETCH, (rd(FP_VERT_STRETCH) & VERT_KEEP_BITS) | (yres - 1) << VERT_PANEL_SHIFT);
    wr(LVDS_PLL_CNTL, (rd(LVDS_PLL_CNTL) & ~R300_LVDS_SRC_SEL_MASK) | R300_LVDS_SRC_SEL_RMX);
    wr(LVDS_GEN_CNTL, rd(LVDS_GEN_CNTL) | LVDS_ON | LVDS_BLON);   /* the BIOS left it on; keep it so */
    fifo_wait(2);
    pll_wr(VCLK_ECP_CNTL, vclk);

    struct zaeboot_framebuffer fb = {
        .base = vram_phys, .width = xres, .height = yres, .pitch = pitch_bytes, .bpp = 32,
        .red_shift = 16, .green_shift = 8, .blue_shift = 0,
    };
    report_regs("after");
    fb_init(&fb, (void *)fbv, 0);
    fb_use_shadow();                              /* reading this memory back is very slow: scroll in RAM */
    fb_dev_init();                                /* booted in text mode: /dev/fb0 appears now */
    on = 1;
    keep = at_boot;
    if (!at_boot) {
        task_create("radeon-revert", revert_thread, NULL);
        kprintf("radeon: `echo keep > /proc/radeon` within 15 s, or it goes back to text mode\n");
    }
    kprintf("radeon: %ux%u at 32 bpp on the panel (PLL ref %u, div %x), /dev/fb0 is the IGP's memory\n",
            xres, yres, ref_div, div3);
    return 0;
}

static long radeon_read(struct file *f, void *buf, size_t len)
{
    if (f->pos >= report_len) return 0;
    size_t n = report_len - (size_t)f->pos < len ? report_len - (size_t)f->pos : len;
    memcpy(buf, report + f->pos, n);
    f->pos += n;
    return (long)n;
}

static long radeon_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    if (len >= 2 && memcmp(buf, "on", 2) == 0 && !on)
        if (set_mode(0) != 0) return -EIO;
    if (len >= 4 && memcmp(buf, "keep", 4) == 0 && on) keep = 1;
    return (long)len;
}

static void save_state(size_t vram_len)
{
    for (size_t i = 0; i < NSAVED; i++) saved[i] = rd(saved_regs[i]);
    for (int i = 0; i < 8; i++) {
        saved_surf[i * 3] = rd(SURFACE0_LOWER_BOUND + 0x10 * i);
        saved_surf[i * 3 + 1] = rd(SURFACE0_UPPER_BOUND + 0x10 * i);
        saved_surf[i * 3 + 2] = rd(SURFACE0_INFO + 0x10 * i);
    }
    saved_vclk = pll_rd(VCLK_ECP_CNTL);
    uint32_t dac2 = rd(DAC_CNTL2);
    wr(DAC_CNTL2, dac2 & ~DAC2_PALETTE_ACC_CTL);
    for (uint32_t i = 0; i < 256; i++) { wr(PALETTE_INDEX, i << 16); saved_pal[i] = rd(PALETTE_30_DATA); }
    wr(DAC_CNTL2, dac2);
    saved_vram = kmalloc(vram_len);
    saved_vram_len = saved_vram ? vram_len : 0;
}

static void restore_state(void)
{
    for (size_t i = 0; i < NSAVED; i++)
        if (saved_regs[i] != CRTC_GEN_CNTL) wr(saved_regs[i], saved[i]);
    for (int i = 0; i < 8; i++) {
        wr(SURFACE0_LOWER_BOUND + 0x10 * i, saved_surf[i * 3]);
        wr(SURFACE0_UPPER_BOUND + 0x10 * i, saved_surf[i * 3 + 1]);
        wr(SURFACE0_INFO + 0x10 * i, saved_surf[i * 3 + 2]);
    }
    pll_wr(VCLK_ECP_CNTL, saved_vclk);
    uint32_t dac2 = rd(DAC_CNTL2);
    wr(DAC_CNTL2, dac2 & ~DAC2_PALETTE_ACC_CTL);
    wr(PALETTE_INDEX, 0);
    for (uint32_t i = 0; i < 256; i++) wr(PALETTE_30_DATA, saved_pal[i]);
    wr(DAC_CNTL2, dac2);
    wr(CRTC_GEN_CNTL, saved[0]);                  /* last: back to VGA */
    if (saved_vram) for (size_t i = 0; i < saved_vram_len / 4; i++) fbv[i] = ((uint32_t *)saved_vram)[i];
}

/* What the display registers hold now, for the report. */
static void report_regs(const char *when)
{
    ap("radeon: "); ap(when); ap(": CRTC "); apx(rd(CRTC_GEN_CNTL)); ap(" ext "); apx(rd(CRTC_EXT_CNTL));
    ap(" status "); apx(rd(CRTC_STATUS)); ap(" H "); apx(rd(CRTC_H_TOTAL_DISP)); ap(" V "); apx(rd(CRTC_V_TOTAL_DISP));
    ap("\n        FP "); apx(rd(FP_GEN_CNTL)); ap(" stretch "); apx(rd(FP_HORZ_STRETCH)); ap("/"); apx(rd(FP_VERT_STRETCH));
    ap(" LVDS "); apx(rd(LVDS_GEN_CNTL)); ap(" pll "); apx(rd(LVDS_PLL_CNTL)); ap(" merge "); apx(rd(DISP_MERGE_CNTL));
    ap(" mc "); apx(rd(MC_FB_LOCATION)); ap(" base "); apx(rd(DISPLAY_BASE_ADDR));
    ap("\n        dac "); apx(rd(DAC_CNTL)); ap(" palette");
    static const uint8_t probe[] = { 0x00, 0x07, 0x3F, 0xAA, 0xFF };
    for (size_t i = 0; i < sizeof probe; i++) { wr(PALETTE_INDEX, (uint32_t)probe[i] << 16); ap(" "); apx(rd(PALETTE_30_DATA)); }
    ap("\n");
    report[report_len] = 0;
}

/* 15 s to say "keep", else back to text mode. */
static void revert_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 15 && !keep; i++) task_sleep_ms(1000);
    if (keep) { kprintf("radeon: keeping the mode\n"); return; }
    restore_state();
    fb_init_text();
    on = 0;
    ap("radeon: nobody said keep: back to text mode\n");
    kprintf("radeon: nobody said keep: back to text mode (cat /proc/radeon for what the switch did)\n");
}

static const struct dev_ops radeon_ops = { .read = radeon_read, .write = radeon_write };

void radeon_init(void)
{
    for (size_t i = 0; i < pci_count() && !dev; i++) {
        const struct pci_dev *p = pci_get(i);
        if (p->vendor == 0x1002 && (p->device == 0x5954 || p->device == 0x5955 || p->device == 0x5974 || p->device == 0x5975))
            dev = p;
#ifdef RADEON_TEST
        if (p->vendor == 0x1002 && p->device == 0x5159) dev = p;   /* QEMU's ati-vga (an RV100): the probe path only */
#endif
    }
    if (!dev) return;
    vram_phys = dev->bar[0];
    mmio = vmm_map_mmio(dev->bar[2], 0x10000);
    if (!mmio || !vram_phys) { kprintf("radeon: %04x without its BARs\n", dev->device); return; }

    /* ATI: keep the ROM decoded */
    wr(MPP_TB_CONFIG, (rd(MPP_TB_CONFIG) & 0x00FFFFFFu) | 0x04u << 24);
    uint32_t tom = rd(NB_TOM);
    vram_size = (uint64_t)(((tom >> 16) - (tom & 0xFFFF) + 1) << 6) * 1024;
    if (pll.ref_div == 0) pll.ref_div = pll_rd(PPLL_REF_DIV) & PPLL_REF_DIV_MASK;
    read_bios();
#ifdef RADEON_TEST
    if (!panel.valid) {                 /* QEMU's ati-vga has no LCD table: pretend, to walk set_mode and the revert */
        panel.valid = 1; strcpy(panel.id, "QEMU TEST");
        panel.xres = 1024; panel.yres = 768; panel.hblank = 320; panel.hover = 24; panel.hsync = 136;
        panel.vblank = 38; panel.vover = 3; panel.vsync = 6; panel.clock = 6500;
        if (vram_size < (16u << 20)) vram_size = 16u << 20;   /* ati-vga: BAR0 is 16 MiB, NB_TOM means nothing */
    }
#endif
    if (pll.ref_div == 0) pll.ref_div = pll_rd(PPLL_REF_DIV) & PPLL_REF_DIV_MASK;

    ap("radeon: "); apx(dev->device); ap(" at "); apx(dev->bus); ap(":"); apx(dev->slot);
    ap(", "); apu(vram_size >> 20); ap(" MiB shared memory at "); apx(vram_phys); ap(", registers at "); apx(dev->bar[2]); ap("\n");
    ap("radeon: PLL ref "); apmhz(pll.ref_clk); ap(" MHz div "); apu(pll.ref_div);
    ap(", range "); apmhz(pll.ppll_min); ap(".."); apmhz(pll.ppll_max); ap(" MHz\n");
    if (panel.xres) {
        ap("radeon: panel '"); ap(panel.id); ap("' "); apu(panel.xres); ap("x"); apu(panel.yres);
        if (panel.valid) {
            ap(", "); apmhz(panel.clock); ap(" MHz, blank "); apu(panel.hblank); ap("/"); apu(panel.vblank);
            ap(", sync +"); apu(panel.hover); ap(" w"); apu(panel.hsync); ap(" / +"); apu(panel.vover); ap(" w"); apu(panel.vsync);
        } else ap(", no timings for it in the BIOS");
        if (panel.bios_dividers) { ap(", BIOS dividers "); apu(panel.ref_div); ap("/"); apu(panel.fbk_div); ap("/"); apu(panel.post_div); }
        ap("\n");
    }
    ap("radeon: now CRTC "); apx(rd(CRTC_GEN_CNTL)); ap(" ext "); apx(rd(CRTC_EXT_CNTL)); ap(", FP "); apx(rd(FP_GEN_CNTL));
    ap(", LVDS "); apx(rd(LVDS_GEN_CNTL)); ap(", PLL "); apx(pll_rd(PPLL_REF_DIV)); ap("/"); apx(pll_rd(PPLL_DIV_3)); ap("\n");
    if (panel.valid) { ap("radeon: `echo on > /proc/radeon` switches the panel to "); apu(panel.xres); ap("x"); apu(panel.yres); ap("\n"); }
    report[report_len] = 0;
    kprintf("%s", report);
    vfs_mkdev("/proc/radeon", &radeon_ops, NULL);
    /* booted in text mode (no VBE mode on these laptops): the panel's own mode now */
    if (panel.valid && !fb_have_framebuffer()) set_mode(1);
    radeon_cp_start();              /* the 3D engine, for user space: /dev/radeongpu */
}
