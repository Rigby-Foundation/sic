/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The panel of a Qualcomm phone, through the DSI controller the bootloader
 * left running (video mode, scanning out its splash): MIPI DCS commands,
 * which the controller sends between frames. An LCD like the "creek"'s
 * keeps its backlight in the panel (DCS 0x51, "bl_ctrl_dcs" to Qualcomm's
 * driver), and display off with sleep in (0x28, 0x10) darkens it.
 *
 * The packet goes through the command DMA's FIFO in the test pattern
 * generator, as Qualcomm's dsi_ctrl_hw_cmn_kickoff_fifo_command does, not
 * through a buffer in memory: the display reads memory through the
 * bootloader's SMMU setup, whose addresses we don't know.
 *
 * /dev/panel: reading says "brightness N/MAX on|off" and the controller's
 * state; writing takes "brightness N", "off", "on", "max N" (the panel's
 * top level), "bytes 1|2" (the size of 0x51's argument), "lp 0|1" (send in
 * low-power mode) and "dcs XX [YY ...]" (any command, hex). */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/timer.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

/* registers, from Qualcomm's dsi_ctrl_reg.h (DSI 6G, "dsi-ctrl-hw-v2.x") */
#define DSI_CTRL                0x004   /* bit 0 on, 1 video engine, 2 command engine */
#define DSI_VIDEO_MODE_CTRL     0x010
#define DSI_COMMAND_MODE_DMA_CTRL 0x03c /* 31 broadcast, 30 master, 28 embedded, 26 low power */
#define DSI_DMA_CMD_LENGTH      0x04c
#define DSI_TRIG_CTRL           0x084   /* bits 2:0 the DMA trigger: 4 = software */
#define DSI_CMD_MODE_DMA_SW_TRIGGER 0x090
#define DSI_INT_CTRL            0x110   /* status in the even bits (0: command DMA done), enables in the odd ones */
#define DSI_TEST_PATTERN_GEN_CTRL 0x15c
#define DSI_TPG_CMD_DMA_INIT_VAL 0x17c  /* the command FIFO, a word per write */
#define DSI_TPG_DMA_FIFO_RESET  0x1ec

#define TPG_CMD_FIFO            (1u << 1 | 1u << 2 | 3u << 16)  /* command DMA from the TPG, FIFO mode, its custom pattern */
#define INT_ENABLES             0xaaaaaaaau
#define MAX_PAYLOAD             64

static volatile uint8_t *dsi;
static volatile int busy;
static int level = -1, max_level = 2047, arg_bytes = 2, lp, screen_on = 1, last_err;

static uint32_t rd(uint32_t off) { return mmio_read32(dsi + off); }
static void wr(uint32_t off, uint32_t v) { mmio_write32(dsi + off, v); }
static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) task_sleep_ms(1); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }

/* One DCS write: command byte and arguments. A short write (no or one
 * argument) or a long one; in the controller's layout: the packet header's
 * bytes 1, 2, 0, then flags (last packet, long), then the payload padded
 * with 0xff to a word. */
static int dcs(const uint8_t *b, int n)
{
    if (!dsi || n < 1 || n > MAX_PAYLOAD) return -EINVAL;
    uint8_t pkt[4 + MAX_PAYLOAD + 4];
    int len;
    if (n <= 2) {
        pkt[0] = b[0]; pkt[1] = n == 2 ? b[1] : 0; pkt[2] = n == 2 ? 0x15 : 0x05; pkt[3] = 0x80;
        len = 4;
    } else {
        pkt[0] = (uint8_t)n; pkt[1] = 0; pkt[2] = 0x39; pkt[3] = 0x80 | 0x40;
        memcpy(pkt + 4, b, (size_t)n);
        len = 4 + n;
        while (len & 3) pkt[len++] = 0xff;
    }

    lock();
    uint32_t ctrl = rd(DSI_CTRL), trig = rd(DSI_TRIG_CTRL), dma = rd(DSI_COMMAND_MODE_DMA_CTRL);
    if (!(ctrl & 1)) { unlock(); last_err = -EIO; return -EIO; }      /* the controller is off: no display running */
    wr(DSI_CTRL, ctrl | 1u << 2);                                     /* the command engine, next to video */
    if ((trig & 7) != 4) wr(DSI_TRIG_CTRL, (trig & ~7u) | 4);
    wr(DSI_INT_CTRL, (rd(DSI_INT_CTRL) & INT_ENABLES) | 1);           /* a stale "done" goes */

    wr(DSI_TEST_PATTERN_GEN_CTRL, TPG_CMD_FIFO);
    for (int i = 0; i < len; i += 4)
        wr(DSI_TPG_CMD_DMA_INIT_VAL, (uint32_t)pkt[i] | (uint32_t)pkt[i + 1] << 8 | (uint32_t)pkt[i + 2] << 16 | (uint32_t)pkt[i + 3] << 24);
    if ((len / 4) & 1) wr(DSI_TPG_CMD_DMA_INIT_VAL, 0);              /* the FIFO takes words in pairs */
    uint32_t d = (dma & ~(1u << 31 | 1u << 30 | 1u << 26)) | 1u << 28 | (lp ? 1u << 26 : 0);
    wr(DSI_COMMAND_MODE_DMA_CTRL, d);
    wr(DSI_DMA_CMD_LENGTH, (uint32_t)len);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(DSI_CMD_MODE_DMA_SW_TRIGGER, 1);

    int rc = -ETIMEDOUT;
    uint64_t t0 = timer_ms();
    while (timer_ms() - t0 < 50) {                                    /* sent in the next blanking: well under a frame */
        if (rd(DSI_INT_CTRL) & 1) { rc = 0; break; }
        task_sleep_ms(1);
    }
    wr(DSI_TEST_PATTERN_GEN_CTRL, 0);                                 /* the FIFO emptied, as Qualcomm resets it */
    wr(DSI_TPG_DMA_FIFO_RESET, 1);
    for (int i = 0; i < 10; i++) (void)rd(DSI_TPG_DMA_FIFO_RESET);
    wr(DSI_TPG_DMA_FIFO_RESET, 0);
    wr(DSI_INT_CTRL, (rd(DSI_INT_CTRL) & INT_ENABLES) | 1);
    wr(DSI_COMMAND_MODE_DMA_CTRL, dma);
    wr(DSI_TRIG_CTRL, trig);
    wr(DSI_CTRL, ctrl);
    unlock();
    last_err = rc;
    if (rc) kprintf("panel: DCS %02x (%d bytes) not sent: no DMA done in 50 ms\n", b[0], n);
    return rc;
}

static int set_level(int v)
{
    if (v < 0) v = 0;
    if (v > max_level) v = max_level;
    uint8_t b[3] = { 0x51, (uint8_t)(arg_bytes == 2 ? v >> 8 : v), (uint8_t)v };
    int rc = dcs(b, arg_bytes == 2 ? 3 : 2);
    if (!rc) level = v;
    return rc;
}

static int set_screen(int on)
{
    static const uint8_t display_off = 0x28, sleep_in = 0x10, sleep_out = 0x11, display_on = 0x29;
    int rc;
    if (on) {
        if ((rc = dcs(&sleep_out, 1))) return rc;
        task_sleep_ms(120);                                           /* what MIPI DCS asks after sleep out */
        if ((rc = dcs(&display_on, 1))) return rc;
        if (level >= 0) set_level(level);
    } else {
        if ((rc = dcs(&display_off, 1))) return rc;
        if ((rc = dcs(&sleep_in, 1))) return rc;
        task_sleep_ms(120);
    }
    screen_on = on;
    return 0;
}

static char report[512];

static long panel_read(struct file *f, void *buf, size_t len)
{
    size_t n;
    if (level >= 0) n = (size_t)ksnprintf(report, sizeof report, "brightness %d/%d %s\n", level, max_level, screen_on ? "on" : "off");
    else n = (size_t)ksnprintf(report, sizeof report, "brightness ?/%d %s\n", max_level, screen_on ? "on" : "off");
    if (dsi)
        n += (size_t)ksnprintf(report + n, sizeof report - n, "dsi: CTRL %08x VIDEO %08x DMA_CTRL %08x TRIG %08x INT %08x; %d-byte level, %s; last %s\n",
                              rd(DSI_CTRL), rd(DSI_VIDEO_MODE_CTRL), rd(DSI_COMMAND_MODE_DMA_CTRL), rd(DSI_TRIG_CTRL), rd(DSI_INT_CTRL),
                              arg_bytes, lp ? "low power" : "high speed", last_err ? "failed" : "ok");
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static const char *skip(const char *p, const char *end) { while (p < end && (*p == ' ' || *p == '\t')) p++; return p; }

/* A number at p (decimal, or hex with `hex`); -1 if there is none. */
static long number(const char **pp, const char *end, int hex)
{
    const char *p = skip(*pp, end);
    long v = 0;
    int any = 0;
    for (; p < end; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : hex && (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * (hex ? 16 : 10) + d; any = 1;
        if (v > 0xffffff) return -1;
    }
    *pp = p;
    return any ? v : -1;
}

static int word(const char **pp, const char *end, const char *w)
{
    const char *p = skip(*pp, end);
    size_t n = strlen(w);
    if ((size_t)(end - p) < n || memcmp(p, w, n) != 0 || (p + n < end && p[n] != ' ' && p[n] != '\n')) return 0;
    *pp = p + n;
    return 1;
}

static long panel_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    const char *p = buf, *end = p + len;
    long v;
    int rc;
    if (!dsi) return -ENODEV;
    if (word(&p, end, "brightness")) { if ((v = number(&p, end, 0)) < 0) return -EINVAL; rc = set_level((int)v); }
    else if (word(&p, end, "off")) rc = set_screen(0);
    else if (word(&p, end, "on")) rc = set_screen(1);
    else if (word(&p, end, "max")) { if ((v = number(&p, end, 0)) < 1 || v > 65535) return -EINVAL; max_level = (int)v; rc = 0; }
    else if (word(&p, end, "bytes")) { if ((v = number(&p, end, 0)) != 1 && v != 2) return -EINVAL; arg_bytes = (int)v; rc = 0; }
    else if (word(&p, end, "lp")) { if ((v = number(&p, end, 0)) != 0 && v != 1) return -EINVAL; lp = (int)v; rc = 0; }
    else if (word(&p, end, "dcs")) {
        uint8_t b[MAX_PAYLOAD];
        int n = 0;
        while (n < MAX_PAYLOAD && (v = number(&p, end, 1)) >= 0 && v <= 0xff) b[n++] = (uint8_t)v;
        if (!n) return -EINVAL;
        rc = dcs(b, n);
    } else return -EINVAL;
    return rc ? rc : (long)len;
}

static const struct dev_ops panel_ops = { .read = panel_read, .write = panel_write };

void qcom_panel_init(void)
{
    static const char *const compat[] = { "qcom,dsi-ctrl-hw-v2.4", "qcom,dsi-ctrl-hw-v2.5", "qcom,dsi-ctrl-hw-v2.3", "qcom,dsi-ctrl-hw-v2.2" };
    int n = -1;
    for (size_t i = 0; i < sizeof compat / sizeof compat[0] && n < 0; i++) n = fdt_find_compatible(compat[i]);
    uint64_t base;
    if (n < 0 || fdt_reg(n, 0, &base, NULL) != 0) return;
    dsi = P2V(base);
    uint32_t ctrl = rd(DSI_CTRL);
    vfs_mkdev("/dev/panel", &panel_ops, NULL);
    kprintf("panel: DSI controller at %x (CTRL %08x: %s): /dev/panel\n", (uint32_t)base, ctrl,
            !(ctrl & 1) ? "off" : ctrl & 2 ? "video mode" : "command mode");
}
