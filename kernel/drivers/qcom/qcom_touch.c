/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The touch controller of a "creek" phone: a Synaptics/OmniVision
 * TouchComm TDDI chip (TD4376) on SPI, with no flash of its own: after a
 * reset it sits in its ROM bootloader and the host gives it everything.
 *   1. the firmware: command 0x45 = [length bits 16-23, 13 zero bytes, the
 *      image's ROMBOOT_APP_CODE], all in one transfer (the ROM bootloader
 *      takes the chip select going up as the end of a packet);
 *   2. 0x42 runs it: the chip reports itself again, now in mode 2;
 *   3. the touch config: 0x30 = [2, 1, the image's APP_CONFIG, padding to
 *      8], in pieces of at most 1024 bytes (each further piece starting
 *      with 0x01) with a pause between them;
 *   4. our report layout (0x26): per finger an index, a class, X and Y in
 *      16 bits each, a pressure.
 * The first finger is the pointer (/dev/mouse, absolute, the left button
 * held while it touches), scaled from the chip's range (0x20) to the screen. 
 * Messages: a write is [code, length LE16, payload]; a read gives
 * [0xa5, code, length LE16], then, read again, [0xa5, 0x03, payload, 0x5a].
 * The interrupt line (active low) says a message is waiting. */
#include "asm/fdt.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "string.h"
#include "printf.h"
#include "drivers/mouse.h"

extern int qcom_spi_init(void);
extern int qcom_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len, int more);
extern void qcom_gpio_set(uint32_t tile, int gpio, int value);
extern int qcom_gpio_get(uint32_t tile, int gpio);

#define TLMM_WEST   0x100000
#define TLMM_SOUTH  0x500000
#define GPIO_RESET  86          /* WEST */
#define GPIO_IRQ    105         /* SOUTH, active low */
#define FIRMWARE    "/lib/firmware/omnivision_hdl_firmware.img"

#define MARKER      0xa5
#define CONTINUED   0x03
#define ST_OK       0x01
#define ST_BUSY     0x02
#define REPORT_IDENTIFY 0x10
#define REPORT_TOUCH    0x11
#define REPORT_STATUS   0x1b

static uint8_t msg[4096 + 3];
static uint32_t msg_len;
static uint8_t *image;
static size_t image_len;
static uint32_t max_x = 10799, max_y = 23399;      /* the chip's range, from its application info */

/* ---- messages ---------------------------------------------------------------------- */

static int recv_msg(void)
{
    uint8_t h[4];
    if (qcom_spi_xfer(NULL, h, 4, 0) != 0 || h[0] != MARKER) return -1;
    msg_len = (uint32_t)h[2] | (uint32_t)h[3] << 8;
    if (!msg_len) return h[1];
    if (msg_len + 3 > sizeof msg || qcom_spi_xfer(NULL, msg, msg_len + 3, 0) != 0) return -1;
    if (msg[0] != MARKER || msg[1] != CONTINUED) return -1;
    memmove(msg, msg + 2, msg_len);
    return h[1];
}

/* The next status (busy and idle skipped) within `ms`; reports seen meanwhile
 * are kept in *report_seen (their code). -1 if none came. */
static int wait_status(int ms, int *report_seen)
{
    for (int t = 0; t < ms; t += 2) {
        int code = recv_msg();
        if (code <= 0 || code == ST_BUSY) { task_sleep_ms(2); continue; }
        if (code >= 0x10) { if (report_seen) *report_seen = code; continue; }
        return code;
    }
    return -1;
}

/* A command, in pieces of at most 1024 bytes (or whole, `single`). */
static int send_cmd(uint8_t code, const uint8_t *payload, uint32_t len, int single)
{
    static uint8_t piece[1024];
    if (single) {
        uint8_t *all = kmalloc(len + 3);
        if (!all) return -1;
        all[0] = code; all[1] = (uint8_t)len; all[2] = (uint8_t)(len >> 8);
        memcpy(all + 3, payload, len);
        int rc = qcom_spi_xfer(all, NULL, len + 3, 0);
        kfree(all);
        return rc;
    }
    uint32_t first = len < sizeof piece - 3 ? len : sizeof piece - 3;
    piece[0] = code; piece[1] = (uint8_t)len; piece[2] = (uint8_t)(len >> 8);
    memcpy(piece + 3, payload, first);
    if (qcom_spi_xfer(piece, NULL, 3 + first, 0) != 0) return -1;
    for (uint32_t done = first; done < len; ) {
        uint32_t n = len - done < sizeof piece - 1 ? len - done : sizeof piece - 1;
        piece[0] = 0x01;
        memcpy(piece + 1, payload + done, n);
        task_sleep_ms(3);
        if (qcom_spi_xfer(piece, NULL, 1 + n, 0) != 0) return -1;
        done += n;
    }
    return 0;
}

/* An area of the firmware image by name. */
static const uint8_t *area(const char *name, uint32_t *len)
{
    uint32_t n = image[4] | (uint32_t)image[5] << 8;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = image + 8 + 4 * i;
        uint32_t off = e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
        if ((size_t)off + 0x24 > image_len || memcmp(image + off + 4, name, strlen(name)) != 0) continue;
        const uint8_t *l = image + off + 0x1c;
        *len = l[0] | (uint32_t)l[1] << 8 | (uint32_t)l[2] << 16 | (uint32_t)l[3] << 24;
        if ((size_t)off + 0x24 + *len > image_len) return NULL;
        return image + off + 0x24;
    }
    return NULL;
}

/* ---- bring-up ---------------------------------------------------------------------- */

static int bring_up(void)
{
    struct vnode *fw = vfs_lookup(vfs_root(), FIRMWARE);
    if (!fw || !(image = vfs_read_all(fw, &image_len))) { kprintf("touch: no %s\n", FIRMWARE); return -1; }
    uint32_t code_len, cfg_len;
    const uint8_t *code = area("ROMBOOT_APP_CODE", &code_len), *cfg = area("APP_CONFIG", &cfg_len);
    if (!code || !cfg) { kprintf("touch: the image has no ROMBOOT_APP_CODE or APP_CONFIG\n"); return -1; }

    /* reset: the chip comes back in its ROM bootloader (and says so) */
    qcom_gpio_set(TLMM_WEST, GPIO_RESET, 0);
    task_sleep_ms(20);
    qcom_gpio_set(TLMM_WEST, GPIO_RESET, 1);
    task_sleep_ms(50);
    for (int t = 0; t < 100 && recv_msg() > 0; t++) ;          /* whatever it said */

    uint8_t *dl = kmalloc(code_len + 14);
    if (!dl) return -1;
    memset(dl, 0, 14);
    dl[0] = (uint8_t)((code_len + 14) >> 16);
    memcpy(dl + 14, code, code_len);
    send_cmd(0x45, dl, code_len + 14, 1);
    kfree(dl);
    int st = wait_status(15000, NULL);
    if (st != ST_OK) { kprintf("touch: firmware download: status %d\n", st); return -1; }

    int report = 0;
    send_cmd(0x42, NULL, 0, 0);
    for (int t = 0; t < 2000 && report != REPORT_STATUS; t += 10) {     /* identify, then the HDL status */
        int c = recv_msg();
        if (c >= 0x10) report = c;
        else task_sleep_ms(10);
    }
    if (report != REPORT_STATUS) { kprintf("touch: the firmware did not ask for its config\n"); return -1; }
    uint32_t hdl = (msg[0] >> 3) & 0xf;

    uint32_t pad = (8 - cfg_len % 8) % 8;
    uint8_t *c = kmalloc(cfg_len + 2 + pad);
    if (!c) return -1;
    memset(c, 0, cfg_len + 2 + pad);
    c[0] = (uint8_t)(0x020101u >> (hdl < 3 ? hdl * 8 : 16));
    c[1] = 1;                                                   /* the touch config */
    memcpy(c + 2, cfg, cfg_len);
    send_cmd(0x30, c, cfg_len + 2 + pad, 0);
    kfree(c);
    st = wait_status(5000, NULL);
    if (st != ST_OK) { kprintf("touch: config download: status %d\n", st); return -1; }

    static const uint8_t layout[] = { 0x0f, 0x01, 0x04, 0x01, 0x06, 0x04, 0x07, 0x04, 0x08, 0x10, 0x09, 0x10, 0x0a, 0x08, 0x03, 0x00 };
    send_cmd(0x26, layout, sizeof layout, 0);
    st = wait_status(500, NULL);
    if (st != ST_OK) { kprintf("touch: report layout: status %d\n", st); return -1; }

    send_cmd(0x20, NULL, 0, 0);                                 /* application info: the range */
    if (wait_status(500, NULL) == ST_OK && msg_len >= 36) {
        max_x = msg[32] | (uint32_t)msg[33] << 8;
        max_y = msg[34] | (uint32_t)msg[35] << 8;
    }
    return 0;
}

/* ---- reports ----------------------------------------------------------------------- */

/* [buttons, then per finger: index | class << 4, X LE16, Y LE16, pressure] */
static void touch_report(void)
{
    static int down, last_x, last_y;
    if (msg_len < 7) {                                          /* no finger: let go where it was */
        if (down) mouse_push_abs(last_x, last_y, 0);
        down = 0;
        return;
    }
    uint32_t x = msg[2] | (uint32_t)msg[3] << 8, y = msg[4] | (uint32_t)msg[5] << 8;     /* the first finger */
    if (max_x && max_y) {
        last_x = (int)(x * platform.fb_width / (max_x + 1));
        last_y = (int)(y * platform.fb_height / (max_y + 1));
    }
    if (!down) mouse_push_abs(last_x, last_y, 0);               /* move there, then press */
    mouse_push_abs(last_x, last_y, MOUSE_BTN_LEFT);
    down = 1;
}

static void touch_thread(void *arg)
{
    (void)arg;
    if (bring_up() != 0) return;
    kprintf("touch: TD4376 running (host-download firmware, config), range %ux%u -> the pointer\n", max_x + 1, max_y + 1);
    for (;;) {
        if (qcom_gpio_get(TLMM_SOUTH, GPIO_IRQ) != 0) { task_sleep_ms(5); continue; }
        int code = recv_msg();
        if (code == REPORT_TOUCH) touch_report();
        else if (code < 0) task_sleep_ms(5);
    }
}

void qcom_touch_init(void)
{
    if (fdt_find_compatible("omnivision,tcm-spi") < 0) return;
    if (qcom_spi_init() != 0) return;
    task_create("touch", touch_thread, NULL);
}
