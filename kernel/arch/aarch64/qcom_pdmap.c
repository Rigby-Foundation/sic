/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The protection domain mapper (QMI service 64, "servreg locator"): the
 * remote processors ask it which of their domains provide a service
 * ("wlan/fw" -> msm/modem/wlan_pd). The modem starts its WLAN domain, the
 * one that runs the WLAN firmware and offers WLFW, only once it has its
 * answer. The table is Linux's qcom_pd_mapper's for SM6115-class SoCs
 * (what this phone's modemr.jsn and modemuw.jsn say too). */
#include "asm/qcom_ipc.h"
#include "string.h"
#include "printf.h"

#define SERVREG_LOC_SERVICE     0x40
#define SERVREG_LOC_VERSION     0x101
#define SERVREG_LOC_INSTANCE    0
#define PDMAP_PORT              0x600
#define GET_DOMAIN_LIST         0x21
#define LOC_PFR                 0x24

static const struct domain { const char *name; uint32_t instance; const char *services[3]; } domains[] = {
    { "msm/adsp/audio_pd", 74, { "avs/audio" } },
    { "msm/adsp/root_pd", 74, { 0 } },
    { "msm/adsp/sensor_pd", 74, { 0 } },
    { "msm/cdsp/root_pd", 76, { 0 } },
    { "msm/modem/root_pd", 180, { "gps/gps_service" } },
    { "msm/modem/wlan_pd", 180, { "kernel/elf_loader", "wlan/fw" } },
};
#define NDOMAINS (sizeof domains / sizeof domains[0])

static uint32_t n_asked;
static char last_asked[4][48];

/* Every domain is in "tms/servreg"; each also in its own services. */
static int provides(const struct domain *d, const char *service)
{
    if (!strcmp(service, "tms/servreg")) return 1;
    for (int i = 0; i < 3 && d->services[i]; i++) if (!strcmp(d->services[i], service)) return 1;
    return 0;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void pdmap_rx(uint32_t node, uint32_t port, const uint8_t *msg, size_t len)
{
    if (len < 7 || msg[0] != 0) return;
    uint16_t txn = (uint16_t)(msg[1] | msg[2] << 8), id = (uint16_t)(msg[3] | msg[4] << 8);
    char service[66] = { 0 };
    uint32_t offset = 0;
    for (size_t o = 7; o + 3 <= len; ) {                        /* TLV 1: the name (no length of its own), 0x10: an offset */
        uint16_t l = (uint16_t)(msg[o + 1] | msg[o + 2] << 8);
        if (o + 3 + l > len) break;
        if (msg[o] == 1) memcpy(service, msg + o + 3, l < 65 ? l : 65);
        if (msg[o] == 0x10 && l >= 4) offset = (uint32_t)msg[o + 3] | (uint32_t)msg[o + 4] << 8 | (uint32_t)msg[o + 5] << 16 | (uint32_t)msg[o + 6] << 24;
        o += 3 + (size_t)l;
    }
    uint8_t r[1024];
    size_t n = 7;
    r[0] = 2; put16(r + 1, txn); put16(r + 3, id);
    r[n++] = 2; put16(r + n, 4); n += 2; put16(r + n, 0); put16(r + n + 2, 0); n += 4;     /* success */
    if (id == GET_DOMAIN_LIST) {
        int total = 0, listed = 0;
        uint8_t list[900];
        size_t ln = 1;
        for (size_t i = 0; i < NDOMAINS; i++) {
            if (!provides(&domains[i], service)) continue;
            if ((uint32_t)total++ < offset) continue;
            size_t nl = strlen(domains[i].name);
            list[ln++] = (uint8_t)nl;                           /* in a struct: a length, then the name */
            memcpy(list + ln, domains[i].name, nl); ln += nl;
            put32(list + ln, domains[i].instance); ln += 4;
            list[ln++] = 0;                                     /* no service data */
            put32(list + ln, 0); ln += 4;
            listed++;
        }
        list[0] = (uint8_t)listed;
        r[n++] = 0x10; put16(r + n, 2); n += 2; put16(r + n, (uint32_t)total); n += 2;      /* total domains */
        r[n++] = 0x11; put16(r + n, 2); n += 2; put16(r + n, 1); n += 2;                    /* database revision */
        if (total) { r[n++] = 0x12; put16(r + n, (uint32_t)ln); n += 2; memcpy(r + n, list, ln); n += ln; }
        n_asked++;
        ksnprintf(last_asked[(n_asked - 1) % 4], sizeof last_asked[0], "%s:%d", service, total);
        kprintf("pdmap: %s -> %d domain(s)\n", service, total);
    }
    put16(r + 5, (uint32_t)(n - 7));
    qrtr_sendto(PDMAP_PORT, node, port, r, n);
}

void qcom_pdmap_setup(void)
{
    static int done;
    if (done++) return;
    qrtr_add_server(SERVREG_LOC_SERVICE, SERVREG_LOC_VERSION, SERVREG_LOC_INSTANCE, PDMAP_PORT, pdmap_rx);
}

size_t qcom_pdmap_report(char *buf, size_t len)
{
    size_t n = (size_t)ksnprintf(buf, len, "pdmap: %u lookups;", n_asked);
    for (uint32_t i = 0; i < 4 && i < n_asked && n < len; i++) n += (size_t)ksnprintf(buf + n, len - n, " %s", last_asked[(n_asked - 1 - i) % 4]);
    if (n < len) n += (size_t)ksnprintf(buf + n, len - n, "\n");
    return n;
}
