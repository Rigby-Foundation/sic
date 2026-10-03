/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The WLAN firmware's control service (WLFW, QMI service 69), which the
 * modem offers once it runs the WCN3990-family firmware (wlanmdsp.mbn).
 * The steps Linux's ath10k_qmi takes, one request at a time:
 *   indications on -> host capabilities -> the MSA memory (given to the
 *   WLAN's VMIDs through TrustZone) -> MSA ready -> capabilities (chip,
 *   board) -> the board data file (bdwlan.*) -> calibration report
 * and then the firmware says FW_READY. Turning the radio on (WLAN mode,
 * with the copy engine configuration) comes with the copy engines. */
#include "asm/qcom_ipc.h"
#include "asm/qcom_scm.h"
#include "asm/timer.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

#define WLFW_SERVICE        69
#define WLFW_PORT           0x500
#define MSA_BASE            0x51900000ULL       /* the DT's wlan_msa_region */
#define MSA_SIZE            0x100000
#define BDF_CHUNK           6144                /* QMI_WLFW_MAX_DATA_SIZE_V01 */
#define CLIENT_ID           0x4b4e454c          /* ATH10K_QMI_CLIENT_ID */
#define FW_DIR              "/mnt/modem/image/"

enum {
    IND_REGISTER = 0x20, FW_READY_IND = 0x21, WLAN_MODE = 0x22, WLAN_CFG = 0x23, CAP = 0x24, BDF_DOWNLOAD = 0x25, CAL_REPORT = 0x26,
    MSA_READY_IND = 0x2b, PIN_CONNECT_IND = 0x2c, MSA_INFO = 0x2d, MSA_READY = 0x2e, HOST_CAP = 0x34,
};

enum state { S_WAIT, S_IND, S_HOST_CAP, S_MSA_INFO, S_MSA_READY, S_CAP, S_BDF, S_CAL, S_FW_WAIT, S_FW_READY, S_CFG, S_MODE, S_ON, S_FAILED };
static const char *const names[] = { "waiting for the service", "registering indications", "host capabilities", "MSA info",
    "MSA ready", "capabilities", "board data", "calibration report", "waiting for FW_READY", "firmware ready",
    "copy engine configuration", "mission mode", "radio on", "failed" };

static enum state st;
static uint32_t node, port;
static uint16_t txn = 1;
static uint64_t sent_at;
static char why[96];
static uint32_t chip_id = 0xff, chip_family, board_id = 0xff, soc_id, fw_version;
static char build[64], bdf_name[32];
static struct { uint64_t addr; uint32_t size; uint8_t secure; } regions[4];
static int nregions, msa_fixed;
static uint8_t *bdf;
static size_t bdf_len, bdf_off;
static uint32_t bdf_seg;
static int ind_seen[64];

/* ---- QMI ---------------------------------------------------------------------------------------- */

struct msg { uint8_t b[BDF_CHUNK + 128]; size_t n; };

static void m_start(struct msg *m, uint16_t id)
{
    m->b[0] = 0;                                            /* request */
    m->b[1] = (uint8_t)txn; m->b[2] = (uint8_t)(txn >> 8);
    m->b[3] = (uint8_t)id; m->b[4] = (uint8_t)(id >> 8);
    m->n = 7;
    txn++;
}
static void m_tlv(struct msg *m, uint8_t t, const void *v, uint16_t len)
{
    m->b[m->n++] = t; m->b[m->n++] = (uint8_t)len; m->b[m->n++] = (uint8_t)(len >> 8);
    memcpy(m->b + m->n, v, len); m->n += len;
}
static void m_u8(struct msg *m, uint8_t t, uint8_t v) { m_tlv(m, t, &v, 1); }
static void m_u32(struct msg *m, uint8_t t, uint32_t v) { m_tlv(m, t, &v, 4); }
static void m_send(struct msg *m, enum state next)
{
    uint16_t body = (uint16_t)(m->n - 7);
    m->b[5] = (uint8_t)body; m->b[6] = (uint8_t)(body >> 8);
    qrtr_sendto(WLFW_PORT, node, port, m->b, m->n);
    st = next;
    sent_at = timer_ms();
}

static const uint8_t *tlv(const uint8_t *p, size_t len, uint8_t type, uint16_t *tl)
{
    for (size_t o = 7; o + 3 <= len; ) {
        uint16_t l = (uint16_t)(p[o + 1] | p[o + 2] << 8);
        if (o + 3 + l > len) return NULL;
        if (p[o] == type) { *tl = l; return p + o + 3; }
        o += 3 + (size_t)l;
    }
    return NULL;
}
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void fail(const char *fmt, unsigned a, unsigned b)
{
    ksnprintf(why, sizeof why, fmt, a, b);
    kprintf("wlan: %s: %s\n", names[st], why);
    st = S_FAILED;
}

/* ---- the steps -------------------------------------------------------------------------------------- */

static struct msg m;                                        /* 6 KiB: not on the stack */

static void send_bdf_chunk(void)
{
    size_t n = bdf_len - bdf_off < BDF_CHUNK ? bdf_len - bdf_off : BDF_CHUNK;
    m_start(&m, BDF_DOWNLOAD);
    m_u8(&m, 0x01, 1);                                      /* valid */
    m_u32(&m, 0x10, 0);                                     /* file id */
    m_u32(&m, 0x11, (uint32_t)bdf_len);                     /* total size */
    m_u32(&m, 0x12, bdf_seg);                               /* segment */
    uint8_t *d = m.b + m.n;                                 /* data: a 16-bit length, then the bytes */
    d[0] = 0x13; d[1] = (uint8_t)(n + 2); d[2] = (uint8_t)((n + 2) >> 8);
    d[3] = (uint8_t)n; d[4] = (uint8_t)(n >> 8);
    memcpy(d + 5, bdf + bdf_off, n);
    m.n += 5 + n;
    m_u8(&m, 0x14, bdf_off + n == bdf_len);                 /* end */
    bdf_off += n;
    bdf_seg++;
    m_send(&m, S_BDF);
}

static int load_bdf(void)
{
    char tries[3][24];
    ksnprintf(tries[0], sizeof tries[0], "bdwlan.%03x", board_id);
    ksnprintf(tries[1], sizeof tries[1], "bdwlan.b%02x", board_id & 0xff);
    ksnprintf(tries[2], sizeof tries[2], "bdwlan.bin");
    for (int i = 0; i < 3; i++) {
        char path[64];
        ksnprintf(path, sizeof path, FW_DIR "%s", tries[i]);
        struct vnode *v = vfs_lookup(vfs_root(), path);
        if (v && (bdf = vfs_read_all(v, &bdf_len)) && bdf_len) {
            memcpy(bdf_name, tries[i], sizeof bdf_name);
            kprintf("wlan: board %x: %s, %lu bytes\n", board_id, bdf_name, (unsigned long)bdf_len);
            return 0;
        }
    }
    return -1;
}

static void wlfw_rx(uint32_t from_node, uint32_t from_port, const uint8_t *p, size_t len)
{
    if (from_node != node || from_port != port || len < 7) return;
    uint16_t id = (uint16_t)(p[3] | p[4] << 8), tl;
    if (p[0] == 4) {                                        /* an indication */
        if (id < 64) ind_seen[id]++;
        if (id == FW_READY_IND) { kprintf("wlan: the firmware is ready\n"); if (st < S_FW_READY) st = S_FW_READY; }
        else if (id == MSA_READY_IND) kprintf("wlan: MSA ready (indication)\n");
        else kprintf("wlan: indication %x\n", id);
        return;
    }
    if (p[0] != 2) return;
    const uint8_t *r = tlv(p, len, 2, &tl);
    unsigned result = r && tl >= 4 ? (unsigned)(r[0] | r[1] << 8) : 99, error = r && tl >= 4 ? (unsigned)(r[2] | r[3] << 8) : 99;
    const uint8_t *v;
    switch (id) {
    case IND_REGISTER:
        if (result) { fail("result %u error %u", result, error); return; }
        if ((v = tlv(p, len, 0x10, &tl)) && tl >= 8 && (g32(v) & 2)) { kprintf("wlan: the firmware was ready already\n"); st = S_FW_READY; return; }
        m_start(&m, HOST_CAP);
        m_u32(&m, 0x10, 0);                                 /* daemon support: none */
        m_send(&m, S_HOST_CAP);
        break;
    case HOST_CAP:
        if (result && error != 0x5e) { fail("result %u error %u", result, error); return; }     /* not supported: fine */
        m_start(&m, MSA_INFO);
        { uint64_t a = MSA_BASE; m_tlv(&m, 0x01, &a, 8); }
        m_u32(&m, 0x02, MSA_SIZE);
        m_send(&m, S_MSA_INFO);
        break;
    case MSA_INFO: {
        if (result) { fail("result %u error %u", result, error); return; }
        v = tlv(p, len, 0x03, &tl);
        nregions = 0;
        for (unsigned i = 0; v && tl >= 1 && i < v[0] && i < 4 && 1 + 13 * (i + 1) <= tl; i++) {
            const uint8_t *e = v + 1 + 13 * i;
            regions[i].addr = (uint64_t)g32(e) | (uint64_t)g32(e + 4) << 32;
            regions[i].size = g32(e + 8);
            regions[i].secure = e[12];
            nregions++;
        }
        for (int i = 0; i < nregions; i++) {
            if (regions[i].addr < MSA_BASE || regions[i].addr + regions[i].size > MSA_BASE + MSA_SIZE) { fail("region %u outside the MSA (%x)", (unsigned)i, (unsigned)regions[i].addr); return; }
            uint32_t vm[3] = { SCM_VMID_MSS_MSA, SCM_VMID_WLAN, SCM_VMID_WLAN_CE };
            uint32_t pr[3] = { SCM_PERM_RW, SCM_PERM_RW, SCM_PERM_RW };
            /* On some phones TrustZone gave the MSA to the WLAN already
             * (Linux's qcom,msa-fixed-perm): it refuses, and that is fine. */
            if (qcom_scm_assign_mem(regions[i].addr, regions[i].size, vm, pr, regions[i].secure ? 2 : 3)) msa_fixed++;
        }
        if (msa_fixed) kprintf("wlan: TrustZone kept %d of %d MSA regions as they are (fixed permissions); going on\n", msa_fixed, nregions);
        else kprintf("wlan: %d MSA regions given to the WLAN\n", nregions);
        m_start(&m, MSA_READY);
        m_send(&m, S_MSA_READY);
        break;
    }
    case MSA_READY:
        if (result) { fail("result %u error %u", result, error); return; }
        m_start(&m, CAP);
        m_send(&m, S_CAP);
        break;
    case CAP:
        if (result) { fail("result %u error %u", result, error); return; }
        if ((v = tlv(p, len, 0x10, &tl)) && tl >= 8) { chip_id = g32(v); chip_family = g32(v + 4); }
        if ((v = tlv(p, len, 0x11, &tl)) && tl >= 4) board_id = g32(v);
        if ((v = tlv(p, len, 0x12, &tl)) && tl >= 4) soc_id = g32(v);
        if ((v = tlv(p, len, 0x13, &tl)) && tl >= 4) fw_version = g32(v);
        if ((v = tlv(p, len, 0x14, &tl))) { size_t n = tl < sizeof build - 1 ? tl : sizeof build - 1; memcpy(build, v, n); build[n] = 0; }
        kprintf("wlan: chip %x family %x board %x soc %x, firmware %x %s\n", chip_id, chip_family, board_id, soc_id, fw_version, build);
        if (load_bdf()) { fail("no board data file for board %x in " FW_DIR " (%u)", board_id, 0); return; }
        bdf_off = 0; bdf_seg = 0;
        send_bdf_chunk();
        break;
    case BDF_DOWNLOAD:
        if (result && !(bdf_off == bdf_len && error == 0x01)) { fail("segment %u: error %u", bdf_seg - 1, error); return; }   /* the last one's CRC may fail, as ath10k allows */
        if (bdf_off < bdf_len) { send_bdf_chunk(); break; }
        kprintf("wlan: board data sent (%lu bytes)\n", (unsigned long)bdf_len);
        m_start(&m, CAL_REPORT);
        { uint8_t none = 0; m_tlv(&m, 0x01, &none, 1); }    /* no calibration data of ours */
        m_send(&m, S_CAL);
        break;
    case CAL_REPORT:
        if (result) { fail("result %u error %u", result, error); return; }
        if (st != S_FW_READY) { st = S_FW_WAIT; sent_at = timer_ms(); }
        break;
    case WLAN_CFG:
        if (result) { fail("result %u error %u", result, error); return; }
        m_start(&m, WLAN_MODE);
        m_u32(&m, 0x01, 0);                                 /* mission mode */
        m_u8(&m, 0x10, 0);                                  /* no hardware debug */
        m_send(&m, S_MODE);
        break;
    case WLAN_MODE:
        if (result) { fail("result %u error %u", result, error); return; }
        kprintf("wlan: mission mode on\n");
        st = S_ON;
        qcom_ath_start();
        break;
    default:
        kprintf("wlan: response %x\n", id);
        break;
    }
}

/* The copy engines as the target is to set them up, the services on them,
 * and the shadow registers for their write indices: ath10k snoc.c's
 * target_ce_config_wlan, target_service_to_ce_map_wlan and
 * target_shadow_reg_cfg_map. */
static void send_cfg(void)
{
    static const uint32_t tgt[12][5] = {        /* pipe, direction (1 in, 2 out, 3 both), entries, bytes, flags */
        { 0, 2, 32, 2048, 0 }, { 1, 1, 32, 2048, 0 }, { 2, 1, 64, 2048, 0 }, { 3, 2, 32, 2048, 0 },
        { 4, 2, 256, 256, 1 << 3 /* no interrupts */ }, { 5, 2, 1024, 64, 1 << 3 }, { 6, 3, 32, 16384, 0 },
        { 7, 4, 0, 0, 1 << 3 }, { 8, 1, 32, 2048, 0 }, { 9, 1, 32, 2048, 0 }, { 10, 1, 32, 2048, 0 }, { 11, 1, 32, 2048, 0 },
    };
    static const uint32_t svc[19][3] = {        /* service, direction, pipe */
        { 0x104, 2, 3 }, { 0x104, 1, 2 }, { 0x102, 2, 3 }, { 0x102, 1, 2 }, { 0x101, 2, 3 }, { 0x101, 1, 2 },
        { 0x103, 2, 3 }, { 0x103, 1, 2 }, { 0x100, 2, 3 }, { 0x100, 1, 2 }, { 0x001, 2, 0 }, { 0x001, 1, 2 },
        { 0xfe00, 2, 0 }, { 0xfe00, 1, 2 }, { 0x300, 2, 4 }, { 0x300, 1, 1 }, { 0xfe00, 2, 5 }, { 0x301, 1, 9 },
        { 0x302, 1, 10 },
    };
    static const uint16_t shadow[12][2] = {     /* engine, register (source / destination write index) */
        { 0, 0x3c }, { 3, 0x3c }, { 4, 0x3c }, { 5, 0x3c }, { 7, 0x3c },
        { 1, 0x40 }, { 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 }, { 10, 0x40 }, { 11, 0x40 },
    };
    static const uint32_t svc_log[2][3] = { { 0x600, 1, 11 }, { 0, 0, 0 } };
    uint8_t v[1 + 21 * 12];
    m_start(&m, WLAN_CFG);
    v[0] = 12;
    for (int i = 0; i < 12; i++) memcpy(v + 1 + 20 * i, tgt[i], 20);
    m_tlv(&m, 0x11, v, 1 + 20 * 12);
    v[0] = 21;
    for (int i = 0; i < 19; i++) memcpy(v + 1 + 12 * i, svc[i], 12);
    for (int i = 0; i < 2; i++) memcpy(v + 1 + 12 * (19 + i), svc_log[i], 12);
    m_tlv(&m, 0x12, v, 1 + 12 * 21);
    v[0] = 12;
    for (int i = 0; i < 12; i++) memcpy(v + 1 + 4 * i, shadow[i], 4);
    m_tlv(&m, 0x13, v, 1 + 4 * 12);
    m_send(&m, S_CFG);
}

void qcom_wlan_poll(void)
{
    static int bound;
    if (!bound) { qrtr_bind_port(WLFW_PORT, wlfw_rx); bound = 1; }
    if (st == S_WAIT) {
        if (qrtr_lookup(WLFW_SERVICE, &node, &port)) return;
        kprintf("wlan: WLFW at %u:%u\n", node, port);
        m_start(&m, IND_REGISTER);
        m_u8(&m, 0x10, 1);                                  /* FW ready */
        m_u8(&m, 0x13, 1);                                  /* MSA ready */
        m_u32(&m, 0x15, CLIENT_ID);
        m_send(&m, S_IND);
        return;
    }
    if (st == S_FW_READY) { send_cfg(); return; }
    if (st == S_ON) { qcom_ath_poll(); return; }
    if (st != S_FAILED && st != S_FW_WAIT && timer_ms() - sent_at > 10000) fail("no answer in 10 s (%u%u)", 0, 0);
}

size_t qcom_wlan_report(char *buf, size_t len)
{
    size_t n = (size_t)ksnprintf(buf, len, "wlan: %s%s%s\n", names[st], st == S_FAILED ? ": " : "", st == S_FAILED ? why : "");
    if (chip_id != 0xff && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  chip %x family %x board %x soc %x, firmware %x %s, board data %s\n",
                               chip_id, chip_family, board_id, soc_id, fw_version, build, bdf_name[0] ? bdf_name : "-");
    for (int i = 0; i < nregions && n < len; i++)
        n += (size_t)ksnprintf(buf + n, len - n, "  MSA region %lx+%x%s\n", (unsigned long)regions[i].addr, regions[i].size, regions[i].secure ? " (secure)" : "");
    if (st == S_ON && n < len) n += qcom_ath_report(buf + n, len - n);
    return n;
}

/* ---- /dev/wlan: what zwifi (zde) reads and writes ------------------------------------------- */
/* Read: "state off|starting|ready|failed", "detail <text>", then a
 * "net <dBm> <secure> <ssid>" line per network once scanning exists.
 * Write: "on" (start the modem and the WLAN firmware), "scan",
 * "connect\t<ssid>\t<passphrase>", "disconnect": the last three answer
 * EOPNOTSUPP until the radio can be turned on (the copy engines). */
#include "abi/abi.h"

static char note[96];                       /* the answer to the last command that did not happen */
static char wreport[4096];

static size_t wlan_status(void)
{
    const char *ms = qcom_mss_state(), *word, *detail;
    char d[160];
    int64_t bits = qcom_smp2p_in("slave-kernel");
    if (bits >= 0 && (bits & 1)) { word = "failed"; detail = "the modem crashed (cat /dev/mss for why)"; }
    else if (!strcmp(ms, "stopped") || !memcmp(ms, "image ok", 8) || !strcmp(ms, "no modem here")) {
        word = "off"; detail = strcmp(ms, "no modem here") ? "the Wi-Fi is off" : "no modem on this machine";
    } else if (!strcmp(ms, "starting") || !memcmp(ms, "loading", 7) || !strcmp(ms, "authenticating")) {
        word = "starting"; ksnprintf(d, sizeof d, "the modem: %s", ms); detail = d;
    } else if (!strcmp(ms, "running")) {
        int a = st == S_ON ? qcom_ath_state() : 0;
        if (st == S_ON && a > 0) { word = "ready"; detail = a == 2 ? "the radio is on; scanning" : "the radio is on"; }
        else if (st == S_ON && a < 0) { word = "failed"; detail = "the radio did not come up (cat /dev/mss for why)"; }
        else if (st == S_ON) { word = "starting"; detail = "the radio: copy engines, HTC, WMI"; }
        else if (st == S_FAILED) { word = "failed"; ksnprintf(d, sizeof d, "the WLAN firmware: %s", why); detail = d; }
        else { word = "starting"; ksnprintf(d, sizeof d, "the WLAN firmware: %s", names[st]); detail = d; }
    } else { word = "failed"; ksnprintf(d, sizeof d, "the modem: %s", ms); detail = d; }
    size_t n = (size_t)ksnprintf(wreport, sizeof wreport, "state %s\ndetail %s\n", word, detail);
    if (note[0] && n < sizeof wreport) n += (size_t)ksnprintf(wreport + n, sizeof wreport - n, "note %s\n", note);
    if (chip_id != 0xff && n < sizeof wreport)
        n += (size_t)ksnprintf(wreport + n, sizeof wreport - n, "chip %x board %x firmware %x %s\n", chip_id, board_id, fw_version, build);
    if (st == S_ON && n < sizeof wreport) n += qcom_ath_nets(wreport + n, sizeof wreport - n);
    return n;
}

static long wlan_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) wlan_status();
    size_t n = strlen(wreport);
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, wreport + f->pos, len);
    f->pos += len;
    return (long)len;
}

static long wlan_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    char cmd[16] = { 0 };
    size_t i = 0;
    for (; i < len && i < sizeof cmd - 1; i++) {
        char ch = ((const char *)buf)[i];
        if (ch == '\n' || ch == '\t' || ch == ' ') break;
        cmd[i] = ch;
    }
    if (!strcmp(cmd, "on")) {
        note[0] = 0;
        int rc = qcom_mss_start();
        return rc && rc != -EBUSY ? rc : (long)len;
    }
    if (!strcmp(cmd, "scan")) {
        note[0] = 0;
        if (st == S_ON && qcom_ath_scan() == 0) return (long)len;
        ksnprintf(note, sizeof note, "the radio is not on yet");
        return -EBUSY;
    }
    if (!strcmp(cmd, "connect") || !strcmp(cmd, "disconnect") || !strcmp(cmd, "off")) {
        ksnprintf(note, sizeof note, "'%s' is not written yet: scanning is, connecting comes next", cmd);
        return -EOPNOTSUPP;
    }
    return -EINVAL;
}

static const struct dev_ops wlan_ops = { .read = wlan_read, .write = wlan_write };

void qcom_wlan_init(void) { vfs_mkdev("/dev/wlan", &wlan_ops, NULL); }
