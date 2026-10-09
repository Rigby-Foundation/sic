/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Requests to a Qualcomm SoC's resource power manager (RPM): what turns
 * the PMIC's regulators on and sets their voltages, when they are not the
 * apps processor's to drive (the WLAN's: PM6125 L8, L16, L17, L23).
 * Linux's way: GLINK over the RPM's message RAM (qcom_glink_rpm: two
 * FIFOs a table at the RAM's end names, word accesses, no intents), the
 * channel "rpm_requests", smd-rpm's requests with key/value pairs
 * (qcom_smd-regulator: "swen", "uv"). Synchronous and polled: a handful of
 * requests, when the WLAN is powered up. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/qcom_ipc.h"
#include "asm/timer.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"

#define MSG_RAM         0x045f0000ULL
#define MSG_RAM_SIZE    0x7000
#define TOC_MAGIC       0x67727430      /* "grt0" */
#define TX_FIFO_ID      0x61703272      /* "ap2r" */
#define RX_FIFO_ID      0x72326170      /* "r2ap" */
#define APCS_IPC        (0x0f111000ULL + 8)
#define IPC_RPM_BIT     0

enum { CMD_VERSION = 0, CMD_VERSION_ACK = 1, CMD_OPEN = 2, CMD_CLOSE = 3, CMD_OPEN_ACK = 4, CMD_TX_DATA = 9, CMD_CLOSE_ACK = 11,
       CMD_TX_DATA_CONT = 12, CMD_READ_NOTIF = 13, CMD_SIGNALS = 15 };

static volatile uint8_t *ram;
static volatile uint32_t *tx_tail, *tx_head, *rx_tail, *rx_head;
static volatile uint8_t *tx_fifo, *rx_fifo;
static uint32_t tx_len, rx_len;
static int up, chan_open, rcid;
static char status[96] = "not used";

static void kick(void) { mmio_write32((volatile uint8_t *)P2V(APCS_IPC), 1u << IPC_RPM_BIT); }

static uint32_t rx_avail(void) { uint32_t h = *rx_head, t = *rx_tail; return h >= t ? h - t : rx_len - t + h; }

static void rx_peek(void *dst, uint32_t off, uint32_t n)              /* n a multiple of 4 */
{
    uint32_t t = *rx_tail + off;
    while (t >= rx_len) t -= rx_len;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = mmio_read32(rx_fifo + t);
        memcpy((uint8_t *)dst + i, &w, 4);
        t += 4; if (t >= rx_len) t = 0;
    }
}

static void rx_advance(uint32_t n) { uint32_t t = *rx_tail + n; while (t >= rx_len) t -= rx_len; *rx_tail = t; }

static int tx(const void *data, uint32_t n)                            /* n a multiple of 8 */
{
    uint32_t h = *tx_head, t = *tx_tail;
    uint32_t avail = t <= h ? tx_len - h + t : t - h;
    if (avail < n + 8) return -1;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w; memcpy(&w, (const uint8_t *)data + i, 4);
        mmio_write32(tx_fifo + h, w);
        h += 4; if (h >= tx_len) h = 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    *tx_head = h;
    __asm__ volatile("dsb sy" ::: "memory");
    kick();
    return 0;
}

static int tx_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
    uint32_t m[2] = { cmd | (uint32_t)p1 << 16, p2 };
    return tx(m, 8);
}

static uint8_t reply[256];
static int reply_len = -1;

/* Whatever the RPM sent; returns once nothing is left. */
static void rx(void)
{
    for (;;) {
        uint32_t avail = rx_avail();
        if (avail < 8) return;
        uint32_t m[2];
        rx_peek(m, 0, 8);
        uint16_t cmd = (uint16_t)m[0], p1 = (uint16_t)(m[0] >> 16);
        uint32_t p2 = m[1];
        switch (cmd) {
        case CMD_VERSION: rx_advance(8); tx_cmd(CMD_VERSION_ACK, 1, 0); break;
        case CMD_VERSION_ACK: rx_advance(8); up = 1; break;
        case CMD_OPEN: {
            uint32_t len = 8 + (((p2 & 0xffff) + 7) & ~7u);
            if (avail < len) return;
            rcid = p1;
            rx_advance(len);
            tx_cmd(CMD_OPEN_ACK, (uint16_t)rcid, 0);
            chan_open |= 2;
            break;
        }
        case CMD_OPEN_ACK: rx_advance(8); chan_open |= 1; break;
        case CMD_TX_DATA: case CMD_TX_DATA_CONT: {
            if (avail < 16) return;
            uint32_t h[4];
            rx_peek(h, 0, 16);
            uint32_t chunk = h[2], total = 16 + ((chunk + 7) & ~7u);
            if (avail < total) return;
            if (chunk <= sizeof reply) { rx_peek(reply, 16, (chunk + 3) & ~3u); reply_len = (int)chunk; }
            rx_advance(total);
            break;
        }
        case CMD_READ_NOTIF: rx_advance(8); kick(); break;
        default: rx_advance(8); break;                     /* close, signals: nothing to do here */
        }
    }
}

static int wait_for(int *flag, int want, int ms)
{
    uint64_t t0 = timer_ms();
    while ((*flag & want) != want) {
        rx();
        if (timer_ms() - t0 > (uint64_t)ms) return -1;
        task_sleep_ms(1);
    }
    return 0;
}

static int link_up(void)
{
    if (chan_open == 3) return 0;
    ram = P2V(MSG_RAM);
    const volatile uint8_t *toc = ram + MSG_RAM_SIZE - 256;
    if (mmio_read32(toc) != TOC_MAGIC) { ksnprintf(status, sizeof status, "no GLINK table in the RPM's message RAM"); return -1; }
    uint32_t n = mmio_read32(toc + 4);
    for (uint32_t i = 0; i < n && i < 20; i++) {
        uint32_t id = mmio_read32(toc + 8 + 12 * i), off = mmio_read32(toc + 12 + 12 * i), size = mmio_read32(toc + 16 + 12 * i);
        if (off + size > MSG_RAM_SIZE) continue;
        if (id == TX_FIFO_ID) { tx_tail = (volatile uint32_t *)(ram + off); tx_head = tx_tail + 1; tx_fifo = ram + off + 8; tx_len = size; }
        if (id == RX_FIFO_ID) { rx_tail = (volatile uint32_t *)(ram + off); rx_head = rx_tail + 1; rx_fifo = ram + off + 8; rx_len = size; }
    }
    if (!tx_fifo || !rx_fifo) { ksnprintf(status, sizeof status, "no FIFOs in the RPM's GLINK table"); return -1; }
    *tx_head = 0;                                           /* as Linux's qcom_glink_rpm_probe */
    *rx_tail = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    tx_cmd(CMD_VERSION, 1, 0);
    if (wait_for(&up, 1, 1000)) { ksnprintf(status, sizeof status, "the RPM did not answer GLINK's version"); return -1; }
    static const char name[16] = "rpm_requests";
    uint8_t m[8 + 16] = { 0 };
    uint32_t h[2] = { CMD_OPEN | 1u << 16, 13 };
    memcpy(m, h, 8); memcpy(m + 8, name, 16);
    tx(m, sizeof m);
    if (wait_for(&chan_open, 3, 1000)) { ksnprintf(status, sizeof status, "rpm_requests did not open (%d)", chan_open); return -1; }
    ksnprintf(status, sizeof status, "rpm_requests open");
    return 0;
}

/* One request: type and id of a resource, key/value words; 0 if the RPM
 * said yes. */
static uint32_t msg_id = 1;

static int request(uint32_t type, uint32_t id, const uint32_t *kv, int nkv)
{
    uint32_t pkt[2 + 5 + 3 * 4];
    uint32_t data = (uint32_t)nkv * 12;
    pkt[0] = 0x00716572;                                    /* "req" */
    pkt[1] = 20 + data;
    pkt[2] = msg_id++; pkt[3] = 0 /* active set */; pkt[4] = type; pkt[5] = id; pkt[6] = data;
    for (int i = 0; i < nkv; i++) { pkt[7 + 3 * i] = kv[2 * i]; pkt[8 + 3 * i] = 4; pkt[9 + 3 * i] = kv[2 * i + 1]; }
    uint32_t len = 28 + data;
    uint8_t m[16 + sizeof pkt + 8];
    uint32_t h[4] = { CMD_TX_DATA | 1u << 16, 0, len, 0 };  /* intentless: no intent id */
    memcpy(m, h, 16);
    memcpy(m + 16, pkt, len);
    uint32_t total = 16 + ((len + 7) & ~7u);
    memset(m + 16 + len, 0, total - 16 - len);
    reply_len = -1;
    if (tx(m, total)) return -1;
    uint64_t t0 = timer_ms();
    while (reply_len < 0) { rx(); if (timer_ms() - t0 > 1000) return -2; task_sleep_ms(1); }
    /* the answer: "req", length, then messages: "msg#" (ours), "err" with a string */
    for (int o = 8; o + 8 <= reply_len; ) {
        uint32_t type2, l;
        memcpy(&type2, reply + o, 4); memcpy(&l, reply + o + 4, 4);
        if (type2 == 0x00727265) return -3;                 /* "err" */
        o += 8 + (int)((l + 3) & ~3u);
    }
    return 0;
}

#define RPM_LDOA    0x616f646c      /* "ldoa" */
#define KEY_SWEN    0x6e657773      /* "swen" */
#define KEY_UV      0x00007675      /* "uv" */

/* The WLAN's regulators on: what icnss/ath10k vote (the DT's voltages). */
int qcom_rpm_wlan_power(void)
{
    if (link_up()) { kprintf("rpm: %s\n", status); return -1; }
    static const struct { uint32_t ldo, uv; } rails[] = {
        { 8, 640000 },      /* vdd-0.8-cx-mx (the DT's qcom,vdd-cx-mx-config) */
        { 16, 1800000 },    /* vdd-1.8-xo */
        { 17, 1304000 },    /* vdd-1.3-rfa */
        { 23, 3312000 },    /* vdd-3.3-ch0 (qcom,vdd-3.3-ch0-config's top) */
    };
    int bad = 0;
    char res[64] = "";
    size_t rn = 0;
    for (size_t i = 0; i < sizeof rails / sizeof rails[0]; i++) {
        uint32_t kv[4] = { KEY_UV, rails[i].uv, KEY_SWEN, 1 };
        int rc = request(RPM_LDOA, rails[i].ldo, kv, 2);
        if (rc) bad++;
        rn += (size_t)ksnprintf(res + rn, sizeof res - rn, " L%u:%s", rails[i].ldo, rc == 0 ? "on" : rc == -3 ? "refused" : rc == -2 ? "no answer" : "?");
    }
    ksnprintf(status, sizeof status, "WLAN rails%s", res);
    kprintf("rpm: %s\n", status);
    return bad ? -1 : 0;
}

size_t qcom_rpm_report(char *buf, size_t len) { return (size_t)ksnprintf(buf, len, "  rpm: %s\n", status); }
