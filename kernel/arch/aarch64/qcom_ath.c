/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The WLAN on a Qualcomm phone's SoC (WCN3990 family): the apps processor's
 * side, after the modem runs its firmware and WLFW said FW_READY and took
 * the copy engine configuration (qcom_wlan.c). As Linux's ath10k (snoc,
 * ce, htc, wmi-tlv) does it:
 *   SMMU      the WLAN's stream (0x1a0) to physical addresses, through a
 *             context bank with translation off where the hypervisor
 *             will not have an S2CR bypass (arm-smmu-qcom's quirk)
 *   CE        12 copy engines at 0x0c800000: rings of 16-byte descriptors
 *             in memory, write indices in registers (or shadow ones), read
 *             indices the target keeps in memory (RRI)
 *   HTC       endpoints over the pipes: READY, connect HTT and WMI, setup
 *             complete; WMI's flow control by credits
 *   WMI-TLV   service ready -> init -> ready (the MAC address), then a
 *             vdev and scans; beacons and probe responses come as
 *             management frames, and make the network list
 * Polled from the IPC thread: no interrupts. Memory the WLAN reads and
 * writes is mapped uncached (it does not snoop the CPU's caches). */
#include "asm/memlayout.h"
#include "asm/qcom_ipc.h"
#include "asm/qcom_scm.h"
#include "asm/timer.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "endian.h"
#include "string.h"
#include "printf.h"

#define CE_BASE         0x0c800000ULL
#define SMMU_BASE       0x0c600000ULL
#define WLAN_SID        0x1a0
#define WLAN_SID_MASK   0x1
#define SMMU_NUM_SMR    0x32        /* the DT's qcom,num-smr-override: the hypervisor keeps the rest */
#define SMMU_NUM_CB     0x30        /* qcom,num-context-banks-override */

static volatile uint8_t *ce_mmio, *smmu;
static char why[96];
static int stage;                    /* see stage_names */
static const char *const stage_names[] = { "off", "copy engines up, waiting for HTC ready", "connecting HTT", "connecting WMI",
    "waiting for WMI service ready", "waiting for WMI ready", "radio on", "failed" };
enum { A_OFF, A_HTC_READY, A_CONN_HTT, A_CONN_WMI, A_SVC_READY, A_WMI_READY, A_UP, A_FAILED };
static uint8_t mac[6];
static char smmu_note[96];

static uint32_t rd(uint32_t off) { return mmio_read32(ce_mmio + off); }
static void wr(uint32_t off, uint32_t v) { mmio_write32(ce_mmio + off, v); }

static void fail(const char *what)
{
    ksnprintf(why, sizeof why, "%s", what);
    kprintf("ath: %s\n", what);
    stage = A_FAILED;
}

/* ---- memory the WLAN reaches: one uncached pool ---------------------------------------- */

#define POOL_SIZE (1024 * 1024)
static uint64_t pool_phys;
static uint8_t *pool;
static size_t pool_used;

static void *dma_alloc(size_t size, uint64_t *phys)
{
    size_t off = (pool_used + 63) & ~(size_t)63;
    if (off + size > POOL_SIZE) return NULL;
    pool_used = off + size;
    *phys = pool_phys + off;
    memset(pool + off, 0, size);
    return pool + off;
}

/* ---- SMMU --------------------------------------------------------------------------------- */

static uint32_t sr(uint32_t off) { return mmio_read32(smmu + off); }
static void sw(uint32_t off, uint32_t v) { mmio_write32(smmu + off, v); }

static int smmu_setup(void)
{
    smmu = P2V(SMMU_BASE);
    uint32_t id0 = sr(0x20), id1 = sr(0x24), scr0 = sr(0);
    uint32_t pgsize = id1 & (1u << 31) ? 0x10000 : 0x1000, numpage = 1u << (((id1 >> 28) & 7) + 1);
    uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
    int exids = (id0 & (1u << 8)) && (scr0 & (1u << 3));
    if (nsmr > SMMU_NUM_SMR) nsmr = SMMU_NUM_SMR;
    if (ncb > SMMU_NUM_CB) ncb = SMMU_NUM_CB;
    /* already matched (the bootloader's, or an earlier run)? */
    for (uint32_t i = 0; i < nsmr; i++) {
        uint32_t smr = sr(0x800 + 4 * i), s2cr = sr(0xc00 + 4 * i);
        int valid = exids ? (s2cr >> 10) & 1 : smr >> 31;
        uint32_t id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;
        if (valid && ((id ^ WLAN_SID) & ~mask & 0x7fff) == 0) {
            ksnprintf(smmu_note, sizeof smmu_note, "stream %x matched by SMR %u already (S2CR %08x)", WLAN_SID, i, s2cr);
            kprintf("ath: %s\n", smmu_note);
            return 0;
        }
    }
    int free = -1;
    for (uint32_t i = 0; i < nsmr && free < 0; i++) {
        uint32_t smr = sr(0x800 + 4 * i), s2cr = sr(0xc00 + 4 * i);
        int valid = exids ? (s2cr >> 10) & 1 : smr >> 31;
        if (!valid) free = (int)i;
    }
    if (free < 0) { ksnprintf(smmu_note, sizeof smmu_note, "no free stream match register"); return -1; }
    uint32_t s2 = 0xc00 + 4 * (uint32_t)free;
    sw(s2, 1u << 16 | 0xff | (exids ? 1u << 10 : 0));      /* bypass, if the hypervisor lets it be */
    uint32_t back = sr(s2);
    if (((back >> 16) & 3) != 1) {                          /* it does not: a context bank with translation off */
        uint32_t cb = ncb - 1;
        mmio_write32(smmu + (numpage + cb) * pgsize + 0, 0);              /* CB SCTLR: off */
        mmio_write32(smmu + pgsize + 4 * cb, 1u << 16);                    /* GR1 CBAR: S1 (bypassed), S2 bypass */
        sw(s2, 0u << 16 | cb | (exids ? 1u << 10 : 0));                    /* translate, through it */
        ksnprintf(smmu_note, sizeof smmu_note, "stream %x: SMR %d, context bank %u with translation off", WLAN_SID, free, cb);
    } else {
        ksnprintf(smmu_note, sizeof smmu_note, "stream %x: SMR %d, bypass", WLAN_SID, free);
    }
    sw(0x800 + 4 * (uint32_t)free, (exids ? 0 : 1u << 31) | (uint32_t)WLAN_SID_MASK << 16 | WLAN_SID);
    __asm__ volatile("dsb sy" ::: "memory");
    kprintf("ath: %s\n", smmu_note);
    return 0;
}

/* ---- copy engines ------------------------------------------------------------------------- */

#define CE_COUNT 12
#define CE_BASE_OF(n)   (0x240000 + 0x1000 * (n))
#define SR_BASE_LO      0x00
#define SR_BASE_HI      0x04
#define SR_SIZE         0x08
#define DR_BASE_LO      0x0c
#define DR_BASE_HI      0x10
#define DR_SIZE         0x14
#define CTRL1           0x18
#define HOST_IE         0x2c
#define HOST_IS         0x30
#define MISC_IE         0x34
#define SR_WR_INDEX     0x3c
#define DST_WR_INDEX    0x40
#define CURRENT_SRRI    0x44
#define CURRENT_DRRI    0x48
#define SRC_WM          0x4c
#define DST_WM          0x50
#define CE_RRI_LOW      0x24c004
#define CE_RRI_HIGH     0x24c008

struct pipe {
    uint32_t src_n, dst_n, src_max, dst_buf, nbufs, dis_intr;
    volatile uint8_t *src, *dst;            /* descriptors: 16 bytes each */
    uint64_t src_phys, dst_phys;
    uint32_t src_wr, dst_wr, dst_sw;
    uint8_t **txbuf; uint64_t *txbuf_phys;  /* one per source slot */
    uint8_t **rxbuf; uint64_t *rxbuf_phys;  /* posted receive buffers */
    uint16_t *rx_slot;                      /* which buffer sits in each destination slot */
    uint32_t rx_count, tx_count;
};
static struct pipe pipes[CE_COUNT];

/* The host side of each engine, ath10k's host_ce_config_wlan (fewer
 * buffers posted than the rings hold). */
static const struct { uint32_t src_n, src_max, dst_n, dis_intr, nbufs; } host_cfg[CE_COUNT] = {
    { 16, 2048, 0, 0, 0 },          /* 0: HTC control out */
    { 0, 2048, 512, 0, 32 },        /* 1: HTT + HTC in */
    { 0, 2048, 64, 0, 32 },         /* 2: WMI in */
    { 32, 2048, 0, 0, 0 },          /* 3: WMI out */
    { 64, 256, 0, 1, 0 },           /* 4: HTT out (no interrupts) */
    { 0, 512, 512, 0, 32 },         /* 5: HTT in */
    { 0, 0, 0, 0, 0 },              /* 6: the target's own */
    { 2, 2048, 2, 0, 2 },           /* 7: diagnostics */
    { 0, 2048, 128, 0, 16 },        /* 8: target to uMC */
    { 0, 2048, 512, 0, 16 },        /* 9: HTT in */
    { 0, 2048, 512, 0, 16 },        /* 10: HTT in */
    { 0, 2048, 512, 0, 16 },        /* 11: pktlog */
};

static volatile uint32_t *rri;              /* read indices the target writes: src low 16, dst high 16 */
static uint64_t rri_phys;

/* Source write indices go through shadow registers on this chip. */
static uint32_t shadow_src_wr(int ce)
{
    switch (ce) { case 0: return 0x32000; case 3: return 0x3200c; case 4: return 0x32010; case 5: return 0x32014; case 7: return 0x3201c; }
    return CE_BASE_OF(ce) + SR_WR_INDEX;
}

static void post_rx(int ce, uint32_t b)
{
    struct pipe *p = &pipes[ce];
    volatile uint8_t *d = p->dst + 16 * p->dst_wr;
    uint64_t a = p->rxbuf_phys[b];
    mmio_write32(d, (uint32_t)a);
    mmio_write32(d + 4, (uint32_t)(a >> 32) & 0x1f);
    mmio_write32(d + 8, 0);                                 /* nbytes 0: not done */
    p->rx_slot[p->dst_wr] = (uint16_t)b;
    p->dst_wr = (p->dst_wr + 1) & (p->dst_n - 1);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(CE_BASE_OF(ce) + DST_WR_INDEX, p->dst_wr);
}

static int ce_init(void)
{
    rri = dma_alloc(CE_COUNT * 4, &rri_phys);
    if (!rri) return -1;
    wr(CE_RRI_LOW, (uint32_t)rri_phys);
    wr(CE_RRI_HIGH, (uint32_t)(rri_phys >> 32) & 0x1f);
    for (int i = 0; i < CE_COUNT; i++) wr(CE_BASE_OF(i) + CTRL1, rd(CE_BASE_OF(i) + CTRL1) | 1u << 19);   /* read indices to memory */
    for (int i = 0; i < CE_COUNT; i++) {
        struct pipe *p = &pipes[i];
        uint32_t b = CE_BASE_OF(i);
        p->src_n = host_cfg[i].src_n; p->dst_n = host_cfg[i].dst_n; p->src_max = host_cfg[i].src_max;
        p->dis_intr = host_cfg[i].dis_intr; p->nbufs = host_cfg[i].nbufs; p->dst_buf = host_cfg[i].src_max;
        if (p->src_n) {
            p->src = dma_alloc(16 * p->src_n, &p->src_phys);
            p->txbuf = kzalloc(sizeof(uint8_t *) * p->src_n);
            p->txbuf_phys = kzalloc(sizeof(uint64_t) * p->src_n);
            if (!p->src || !p->txbuf || !p->txbuf_phys) return -1;
            if (i == 0 || i == 3 || i == 7)
                for (uint32_t k = 0; k < p->src_n; k++)
                    if (!(p->txbuf[k] = dma_alloc(p->src_max, &p->txbuf_phys[k]))) return -1;
            p->src_wr = rd(b + SR_WR_INDEX) & (p->src_n - 1);
            wr(b + SR_BASE_LO, (uint32_t)p->src_phys);
            wr(b + SR_BASE_HI, (uint32_t)(p->src_phys >> 32) & 0x1f);
            wr(b + SR_SIZE, p->src_n);
            wr(b + CTRL1, (rd(b + CTRL1) & ~0xffffu) | (p->src_max & 0xffff));    /* dmax */
            wr(b + CTRL1, rd(b + CTRL1) & ~(1u << 17));                           /* no byte swap */
            wr(b + SRC_WM, p->src_n & 0xffff);                                     /* low 0, high n */
        }
        if (p->dst_n) {
            p->dst = dma_alloc(16 * p->dst_n, &p->dst_phys);
            p->rxbuf = kzalloc(sizeof(uint8_t *) * p->nbufs);
            p->rxbuf_phys = kzalloc(sizeof(uint64_t) * p->nbufs);
            p->rx_slot = kzalloc(sizeof(uint16_t) * p->dst_n);
            if (!p->dst || !p->rxbuf || !p->rxbuf_phys || !p->rx_slot) return -1;
            for (uint32_t k = 0; k < p->nbufs; k++)
                if (!(p->rxbuf[k] = dma_alloc(p->dst_buf, &p->rxbuf_phys[k]))) return -1;
            p->dst_wr = rd(b + DST_WR_INDEX) & (p->dst_n - 1);
            p->dst_sw = p->dst_wr;
            wr(b + DR_BASE_LO, (uint32_t)p->dst_phys);
            wr(b + DR_BASE_HI, (uint32_t)(p->dst_phys >> 32) & 0x1f);
            wr(b + DR_SIZE, p->dst_n);
            wr(b + CTRL1, rd(b + CTRL1) & ~(1u << 18));                           /* no byte swap */
            wr(b + DST_WM, p->dst_n & 0xffff);
        }
        /* interrupts as ath10k leaves them: copy complete on (unless
         * disabled), watermarks off, errors on (no line is taken here) */
        if (!p->dis_intr) wr(b + HOST_IE, rd(b + HOST_IE) | 1);
        wr(b + HOST_IS, 0);
        wr(b + MISC_IE, rd(b + MISC_IE) | 0x3e0);
    }
    for (int i = 0; i < CE_COUNT; i++)
        for (uint32_t k = 0; k < pipes[i].nbufs; k++) post_rx(i, k);
    return 0;
}

/* A message into a source ring: copied to the slot's buffer. */
static int ce_send(int ce, uint32_t transfer_id, const void *data, size_t len)
{
    struct pipe *p = &pipes[ce];
    if (!p->src_n || !p->txbuf || len > p->src_max) return -1;
    uint32_t read = (p->dis_intr ? rri[ce] : rd(CE_BASE_OF(ce) + CURRENT_SRRI)) & (p->src_n - 1);
    if (((p->src_wr + 1) & (p->src_n - 1)) == read) return -1;      /* full */
    uint32_t slot = p->src_wr;
    memcpy(p->txbuf[slot], data, len);
    uint64_t a = p->txbuf_phys[slot];
    volatile uint8_t *d = p->src + 16 * slot;
    mmio_write32(d, (uint32_t)a);
    mmio_write32(d + 4, (uint32_t)(a >> 32) & 0x1f);
    mmio_write32(d + 8, (uint32_t)len | (transfer_id << 4 & 0xfff0) << 16);   /* nbytes, flags: the endpoint */
    mmio_write32(d + 12, 0);
    p->src_wr = (slot + 1) & (p->src_n - 1);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(shadow_src_wr(ce), p->src_wr);
    p->tx_count++;
    return 0;
}

/* ---- HTC ---------------------------------------------------------------------------------------- */

enum { SVC_RSVD_CTRL = 0x0001, SVC_WMI_CONTROL = 0x0100, SVC_HTT_DATA = 0x0300 };
enum { HTC_READY = 1, HTC_CONNECT = 2, HTC_CONNECT_RESP = 3, HTC_SETUP_COMPLETE_EX = 5 };
#define EP_COUNT 9

static struct ep { uint16_t svc; int ul, dl, credit_flow; int credits; uint8_t seq; } eps[EP_COUNT];
static int wmi_ep = -1, htt_ep = -1;
static uint32_t credit_size = 2048;
static uint32_t htc_rx, htc_unknown;

static int htc_send(int eid, const void *payload, size_t len)
{
    struct ep *e = &eps[eid];
    uint8_t buf[2048];
    if (len + 8 > sizeof buf) return -1;
    if (e->credit_flow) {
        int need = (int)((len + 8 + credit_size - 1) / credit_size);
        if (e->credits < need) return -2;                  /* wait for credits */
        e->credits -= need;
    }
    buf[0] = (uint8_t)eid; buf[1] = e->credit_flow ? 1 : 0;  /* NEED_CREDIT_UPDATE */
    buf[2] = (uint8_t)len; buf[3] = (uint8_t)(len >> 8);
    buf[4] = 0; buf[5] = e->seq++; buf[6] = buf[7] = 0;
    memcpy(buf + 8, payload, len);
    if (ce_send(e->ul, (uint32_t)eid, buf, len + 8)) {
        if (e->credit_flow) e->credits += (int)((len + 8 + credit_size - 1) / credit_size);
        return -1;
    }
    return 0;
}

static void htc_connect(uint16_t svc)
{
    uint8_t m[8] = { HTC_CONNECT, 0, (uint8_t)svc, (uint8_t)(svc >> 8), 0, 0, 0, 0 };
    uint16_t flags = svc == SVC_WMI_CONTROL ? 1u << 8 : 1u << 3;  /* WMI: our 1 credit, flow control; else none */
    m[4] = (uint8_t)flags; m[5] = (uint8_t)(flags >> 8);
    htc_send(0, m, sizeof m);
}

/* Pipes for a service, ath10k's target_service_to_ce_map_wlan. */
static void svc_pipes(uint16_t svc, int *ul, int *dl)
{
    if (svc == SVC_HTT_DATA) { *ul = 4; *dl = 1; }
    else if (svc == SVC_RSVD_CTRL) { *ul = 0; *dl = 2; }
    else { *ul = 3; *dl = 2; }                               /* WMI */
}

/* ---- WMI ------------------------------------------------------------------------------------------ */

enum {
    WMI_INIT = 0x1, WMI_START_SCAN = 0x3001, WMI_SCAN_CHAN_LIST = 0x3003, WMI_VDEV_CREATE = 0x5001,
    EV_SERVICE_READY = 0x1, EV_READY = 0x2, EV_SCAN = 0x3001, EV_MGMT_RX = 0x7001,
};
enum {
    TAG_ARRAY_UINT32 = 0x10, TAG_ARRAY_BYTE = 0x11, TAG_ARRAY_STRUCT = 0x12, TAG_ARRAY_FIXED_STRUCT = 0x13,
    TAG_SERVICE_READY = 0x20, TAG_HOST_MEM_REQ = 0x22, TAG_READY = 0x23, TAG_SCAN_EVENT = 0x24, TAG_MGMT_RX_HDR = 0x2c,
    TAG_INIT_CMD = 0x4a, TAG_RESOURCE_CONFIG = 0x4b, TAG_HOST_MEMORY_CHUNK = 0x4c, TAG_START_SCAN = 0x4d,
    TAG_SCAN_CHAN_LIST = 0x4f, TAG_CHANNEL = 0x50, TAG_VDEV_CREATE = 0x56,
};

/* Commands wait here for WMI's credits. */
#define WMI_Q 16
static struct { uint8_t *d; size_t n; } wq[WMI_Q];
static int wq_head, wq_len;
static uint32_t wmi_events, wmi_unknown, last_unknown;

static void wmi_flush(void)
{
    while (wq_len && wmi_ep >= 0) {
        if (htc_send(wmi_ep, wq[wq_head].d, wq[wq_head].n)) return;
        kfree(wq[wq_head].d);
        wq_head = (wq_head + 1) % WMI_Q; wq_len--;
    }
}

/* A command: its id, then TLVs the caller built. */
static int wmi_send(uint32_t id, const uint8_t *tlvs, size_t len)
{
    if (wq_len == WMI_Q) return -1;
    uint8_t *m = kmalloc(4 + len);
    if (!m) return -1;
    memcpy(m, &id, 4);
    memcpy(m + 4, tlvs, len);
    int t = (wq_head + wq_len) % WMI_Q;
    wq[t].d = m; wq[t].n = 4 + len; wq_len++;
    wmi_flush();
    return 0;
}

struct tb { uint8_t b[1600]; size_t n; };
static void *tlv_put(struct tb *t, uint16_t tag, size_t len)
{
    uint32_t h = (uint32_t)len | (uint32_t)tag << 16;
    memcpy(t->b + t->n, &h, 4);
    void *v = t->b + t->n + 4;
    memset(v, 0, len);
    t->n += 4 + len;
    return v;
}
static void put32(void *p, int i, uint32_t v) { memcpy((uint8_t *)p + 4 * i, &v, 4); }

/* Memory the firmware asks for in service ready. */
#define MAX_CHUNKS 8
static struct { uint32_t req, size; uint64_t phys; } chunks[MAX_CHUNKS];
static int nchunks;

static void send_init(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_INIT_CMD, 7 * 4);
    put32(c, 0, 1u << 24);                                  /* ABI 1.0 */
    put32(c, 1, 53);
    put32(c, 2, 0x5f414351); put32(c, 3, 0x00004c4d);       /* "QCA_ML" */
    put32(c, 6, (uint32_t)nchunks);
    uint32_t *r = tlv_put(&t, TAG_RESOURCE_CONFIG, 44 * 4);
    static const uint32_t cfg[44] = {
        4, 33, 0, 0, 2, 66, 16, 7, 7, 100, 100, 100, 40, 1 /* native wifi */, 4, 4, 4, 8, 0, 0, 0, 0x400, 2,
        0, 0, 0, 0, 2, 1024 + 32, 2, 1, 0x20, 2, 5, 22, 6, 0, 1, 1, 0, 0, 0, 0, 1u << 9 /* mgmt bundle tx completion */,
    };
    for (int i = 0; i < 44; i++) put32(r, i, cfg[i]);
    uint8_t *arr = tlv_put(&t, TAG_ARRAY_STRUCT, (size_t)nchunks * 20);
    for (int i = 0; i < nchunks; i++) {
        uint32_t h = 16 | (uint32_t)TAG_HOST_MEMORY_CHUNK << 16;
        memcpy(arr + 20 * i, &h, 4);
        put32(arr + 20 * i + 4, 0, chunks[i].req);
        put32(arr + 20 * i + 4, 1, (uint32_t)chunks[i].phys);
        put32(arr + 20 * i + 4, 2, chunks[i].size);
        put32(arr + 20 * i + 4, 3, (uint32_t)(chunks[i].phys >> 32));
    }
    wmi_send(WMI_INIT, t.b, t.n);
}

/* ---- scanning ----------------------------------------------------------------------------------- */

#define MAX_BSS 48
static struct bss { uint8_t bssid[6]; char ssid[33]; int rssi, freq, secure; uint64_t seen; } bsses[MAX_BSS];
static int nbss;
static int vdev_up, scanning, scans;
static uint32_t mgmt_rx;

static const uint16_t chans_2g[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472 };
static const uint16_t chans_5g[] = { 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580,
                                     5600, 5620, 5640, 5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825 };
#define N2G (sizeof chans_2g / sizeof chans_2g[0])
#define N5G (sizeof chans_5g / sizeof chans_5g[0])

static void send_chan_list(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_SCAN_CHAN_LIST, 4);
    put32(c, 0, (uint32_t)(N2G + N5G));
    uint8_t *arr = tlv_put(&t, TAG_ARRAY_STRUCT, (N2G + N5G) * 28);
    for (size_t i = 0; i < N2G + N5G; i++) {
        int is2 = i < N2G;
        uint32_t f = is2 ? chans_2g[i] : chans_5g[i - N2G];
        uint32_t h = 24 | (uint32_t)TAG_CHANNEL << 16;
        uint8_t *ch = arr + 28 * i;
        memcpy(ch, &h, 4);
        put32(ch + 4, 0, f);                                /* mhz */
        put32(ch + 4, 1, f);                                /* band centre */
        put32(ch + 4, 2, 0);
        uint32_t flags = (is2 ? 5u : 4u) | 1u << 11;        /* 11ng/na HT20, HT allowed */
        if (!is2 || f > 2462) flags |= 1u << 7;             /* passive where we may not be allowed to probe */
        if (!is2 && f >= 5260 && f <= 5720) flags |= 1u << 10;   /* DFS */
        put32(ch + 4, 3, flags);
        put32(ch + 4, 4, 0 | 40u << 8 | 40u << 16);         /* min 0, max 20 dBm, reg 20 dBm (0.5 dB units) */
        put32(ch + 4, 5, 0 | 40u << 8);                     /* antenna gain 0, max tx 20 dBm */
    }
    wmi_send(WMI_SCAN_CHAN_LIST, t.b, t.n);
}

static void send_vdev_create(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_VDEV_CREATE, 4 * 5);
    put32(c, 0, 0);                                         /* vdev 0 */
    put32(c, 1, 2);                                         /* station */
    put32(c, 2, 0);
    memcpy(c + 3, mac, 6);
    wmi_send(WMI_VDEV_CREATE, t.b, t.n);
}

static void send_start_scan(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_START_SCAN, 15 * 4 + 6 * 4 + 16);
    static const uint32_t common[15] = {
        0xa000 | 1, 0xa000 | 1, 0, 1,                       /* scan id, requestor, vdev, low priority */
        1 | 2 | 4 | 8 | 16 | 256,                           /* events: started, completed, channels, dequeued */
        50, 150, 50, 500, 0, 0, 0, 20000, 5,                /* dwells, rests, max time, probe delay */
        0x10 ^ 0x20,                                        /* channel stats; probe-request filter (its sense inverted on TLV) */
    };
    for (int i = 0; i < 15; i++) put32(c, i, common[i]);
    put32(c, 15, 0);                                        /* burst duration */
    put32(c, 16, (uint32_t)(N2G + N5G));
    put32(c, 17, 1);                                        /* one BSSID: broadcast */
    put32(c, 18, 1);                                        /* one SSID: the wildcard */
    static const uint8_t ies[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,   /* rates */
                                   50, 4, 0x30, 0x48, 0x60, 0x6c };                       /* extended rates */
    put32(c, 19, sizeof ies);
    put32(c, 20, 3);                                        /* probes */
    uint32_t *ch = tlv_put(&t, TAG_ARRAY_UINT32, (N2G + N5G) * 4);
    for (size_t i = 0; i < N2G + N5G; i++) put32(ch, (int)i, i < N2G ? chans_2g[i] : chans_5g[i - N2G]);
    tlv_put(&t, TAG_ARRAY_FIXED_STRUCT, 36);                /* the wildcard SSID: length 0 */
    uint8_t *b = tlv_put(&t, TAG_ARRAY_FIXED_STRUCT, 8);
    memset(b, 0xff, 6);
    uint8_t *ie = tlv_put(&t, TAG_ARRAY_BYTE, (sizeof ies + 3) & ~3u);
    memcpy(ie, ies, sizeof ies);
    wmi_send(WMI_START_SCAN, t.b, t.n);
    scanning = 1; scans++;
}

/* A beacon or probe response: the network it is. */
static void saw_frame(const uint8_t *f, size_t n, int rssi, int freq)
{
    if (n < 36) return;
    uint8_t fc = f[0];
    if (fc != 0x80 && fc != 0x50) return;                   /* beacon, probe response */
    const uint8_t *bssid = f + 16;
    uint16_t cap = (uint16_t)(f[34] | f[35] << 8);
    char ssid[33] = { 0 };
    int secure = (cap & 0x10) != 0, ds = 0;
    for (size_t o = 36; o + 2 <= n; ) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > n) break;
        const uint8_t *v = f + o + 2;
        if (id == 0 && l <= 32) { memcpy(ssid, v, l); ssid[l] = 0; }
        else if (id == 3 && l >= 1) ds = v[0];
        else if (id == 48) secure = 2;                      /* RSN: WPA2/WPA3 */
        else if (id == 221 && l >= 4 && v[0] == 0 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 1 && secure < 2) secure = 1;
        o += 2 + (size_t)l;
    }
    if (ds) freq = ds == 14 ? 2484 : ds < 14 ? 2407 + 5 * ds : 5000 + 5 * ds;
    for (size_t i = 0; ssid[i]; i++) if ((unsigned char)ssid[i] < 32) ssid[i] = '?';
    struct bss *b = NULL;
    for (int i = 0; i < nbss; i++) if (!memcmp(bsses[i].bssid, bssid, 6)) b = &bsses[i];
    if (!b && nbss < MAX_BSS) b = &bsses[nbss++];
    if (!b) return;
    memcpy(b->bssid, bssid, 6);
    memcpy(b->ssid, ssid, sizeof ssid);
    b->rssi = rssi; b->freq = freq; b->secure = secure; b->seen = timer_ms();
}

/* ---- WMI events -------------------------------------------------------------------------------------- */

static void wmi_event(const uint8_t *m, size_t n)
{
    if (n < 4) return;
    uint32_t id;
    memcpy(&id, m, 4);
    id &= 0xffffff;
    wmi_events++;
    const uint8_t *t = m + 4, *end = m + n;
    /* the TLVs, by tag (first of each) */
    const uint8_t *svc = NULL, *rdy = NULL, *scan = NULL, *rxhdr = NULL, *frame = NULL, *memreqs = NULL;
    uint32_t frame_len = 0, memreqs_len = 0;
    while (t + 4 <= end) {
        uint32_t h; memcpy(&h, t, 4);
        uint32_t len = h & 0xffff, tag = h >> 16;
        if (t + 4 + len > end) break;
        const uint8_t *v = t + 4;
        if (tag == TAG_SERVICE_READY && !svc) svc = v;
        else if (tag == TAG_READY && !rdy) rdy = v;
        else if (tag == TAG_SCAN_EVENT && !scan) scan = v;
        else if (tag == TAG_MGMT_RX_HDR && !rxhdr) rxhdr = v;
        else if (tag == TAG_ARRAY_BYTE && !frame) { frame = v; frame_len = len; }
        else if (tag == TAG_ARRAY_STRUCT && !memreqs) { memreqs = v; memreqs_len = len; }
        t += 4 + len;
    }
    switch (id) {
    case EV_SERVICE_READY: {
        if (stage != A_SVC_READY || !svc) break;
        uint32_t nreq; memcpy(&nreq, svc + 4 * 18, 4);       /* after fw_build_vers, abi (6), 11 more */
        nchunks = 0;
        for (const uint8_t *q = memreqs; q && q + 4 + 16 <= memreqs + memreqs_len && nchunks < MAX_CHUNKS; ) {
            uint32_t h; memcpy(&h, q, 4);
            uint32_t req, unit, info, units;
            memcpy(&req, q + 4, 4); memcpy(&unit, q + 8, 4); memcpy(&info, q + 12, 4); memcpy(&units, q + 16, 4);
            if (info & 4) units = 33 + 1; else if (info & 2) units = 33 + 1; else if (info & 1) units = 4 + 1;
            uint32_t size = unit * units;
            uint64_t ph = pmm_alloc_pages_below((size + PAGE_SIZE - 1) / PAGE_SIZE, 0x100000000ULL);
            if (ph) {
                memset(P2V(ph), 0, (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
                qcom_dma_clean(P2V(ph), size);
                chunks[nchunks].req = req; chunks[nchunks].size = size; chunks[nchunks].phys = ph;
                nchunks++;
            }
            q += 4 + (h & 0xffff);
        }
        kprintf("ath: service ready (%u memory requests, %d given)\n", nreq, nchunks);
        send_init();
        stage = A_WMI_READY;
        break;
    }
    case EV_READY:
        if (!rdy) break;
        memcpy(mac, rdy + 24, 6);                           /* after the ABI version (6 words) */
        kprintf("ath: WMI ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        stage = A_UP;
        break;
    case EV_SCAN:
        if (scan) {
            uint32_t ev; memcpy(&ev, scan, 4);
            if (ev & (2 | 64 | 32)) { scanning = 0; kprintf("ath: scan done (%d networks)\n", nbss); }
        }
        break;
    case EV_MGMT_RX:
        if (rxhdr && frame) {
            uint32_t chan, snr, status, buflen;
            memcpy(&chan, rxhdr, 4); memcpy(&snr, rxhdr + 4, 4); memcpy(&buflen, rxhdr + 16, 4); memcpy(&status, rxhdr + 20, 4);
            mgmt_rx++;
            if (!(status & 1)) saw_frame(frame, buflen < frame_len ? buflen : frame_len, (int)snr - 95, (int)chan);
        }
        break;
    default:
        wmi_unknown++; last_unknown = id;
        break;
    }
}

/* ---- HTC receive ---------------------------------------------------------------------------------------- */

static void htc_rx_msg(const uint8_t *m, size_t n)
{
    if (n < 8) return;
    uint8_t eid = m[0], flags = m[1];
    uint32_t len = (uint32_t)(m[2] | m[3] << 8);
    if (eid >= EP_COUNT || len + 8 > n) return;
    htc_rx++;
    if (flags & 2) {                                        /* a trailer: credit reports */
        uint32_t tl = m[4];
        if (tl <= len) {
            const uint8_t *tr = m + 8 + len - tl;
            for (uint32_t o = 0; o + 2 <= tl; ) {
                uint8_t rid = tr[o], rl = tr[o + 1];
                if (o + 2 + rl > tl) break;
                if (rid == 1)
                    for (uint32_t k = 0; k + 4 <= rl; k += 4) {
                        uint8_t e = tr[o + 2 + k], cr = tr[o + 3 + k];
                        if (e < EP_COUNT) eps[e].credits += cr;
                    }
                o += 2u + rl;
            }
            len -= tl;
        }
    }
    const uint8_t *p = m + 8;
    if (eid == 0) {
        uint16_t id = (uint16_t)(p[0] | p[1] << 8);
        if (id == HTC_READY && stage == A_HTC_READY && len >= 8) {
            credit_size = (uint32_t)(p[4] | p[5] << 8);
            if (!credit_size) credit_size = 2048;
            kprintf("ath: HTC ready (credits %u of %u bytes, %u endpoints)\n", (unsigned)(p[2] | p[3] << 8), credit_size, p[6]);
            htc_connect(SVC_HTT_DATA);
            stage = A_CONN_HTT;
        } else if (id == HTC_CONNECT_RESP && len >= 8) {
            uint16_t svc = (uint16_t)(p[2] | p[3] << 8);
            uint8_t status = p[4], e = p[5];
            if (status || e >= EP_COUNT) { fail("the firmware refused a service connection"); return; }
            eps[e].svc = svc;
            svc_pipes(svc, &eps[e].ul, &eps[e].dl);
            eps[e].credit_flow = svc == SVC_WMI_CONTROL;
            eps[e].credits = svc == SVC_WMI_CONTROL ? 1 : 0;
            if (svc == SVC_HTT_DATA) { htt_ep = e; htc_connect(SVC_WMI_CONTROL); stage = A_CONN_WMI; }
            else if (svc == SVC_WMI_CONTROL) {
                wmi_ep = e;
                uint8_t sc[14] = { HTC_SETUP_COMPLETE_EX, 0 };
                htc_send(0, sc, sizeof sc);
                stage = A_SVC_READY;
                kprintf("ath: HTC: HTT on endpoint %d, WMI on %d\n", htt_ep, wmi_ep);
            }
        }
        return;
    }
    if (eid == wmi_ep) wmi_event(p, len);
    /* HTT messages (and anything else) are not looked at yet */
    else htc_unknown++;
}

static void ce_poll(void)
{
    for (int i = 0; i < CE_COUNT; i++) {
        struct pipe *p = &pipes[i];
        if (!p->dst_n || !p->nbufs) continue;
        for (int k = 0; k < 16; k++) {
            volatile uint8_t *d = p->dst + 16 * p->dst_sw;
            uint32_t w2 = mmio_read32(d + 8);
            uint32_t nbytes = w2 & 0xffff;
            if (!nbytes) break;
            __asm__ volatile("dmb sy" ::: "memory");
            uint32_t b = p->rx_slot[p->dst_sw];
            p->dst_sw = (p->dst_sw + 1) & (p->dst_n - 1);
            p->rx_count++;
            if (i == 1 || i == 2) htc_rx_msg(p->rxbuf[b], nbytes);
            post_rx(i, b);
        }
    }
    wmi_flush();
}

/* ---- the whole --------------------------------------------------------------------------------------- */

static uint8_t pmic_l[4][3];
static int pmic_ok;

/* The WLAN's regulators (PM6125 LDO8, 16, 17, 23): only looked at here;
 * the RPM owns them. EN_CTL, STATUS1, the voltage's low byte. */
static void read_rails(void)
{
    static const uint8_t ldos[4] = { 8, 16, 17, 23 };
    pmic_ok = 1;
    for (int i = 0; i < 4; i++) {
        uint16_t base = (uint16_t)(0x4000 + (ldos[i] - 1) * 0x100);
        if (qcom_pmic_read(0, base + 0x46, &pmic_l[i][0]) || qcom_pmic_read(0, base + 0x08, &pmic_l[i][1]) ||
            qcom_pmic_read(0, base + 0x40, &pmic_l[i][2])) pmic_ok = 0;
    }
}

/* After WLAN mode ON: the SMMU, the engines, HTC. */
int qcom_ath_start(void)
{
    if (stage != A_OFF && stage != A_FAILED) return 0;
    why[0] = 0;
    read_rails();
    ce_mmio = P2V(CE_BASE);
    if (!pool) {
        pool_phys = pmm_alloc_pages_below(POOL_SIZE / PAGE_SIZE, 0x100000000ULL);
        if (!pool_phys) { fail("no memory for the copy engines"); return -1; }
        qcom_dma_clean(P2V(pool_phys), POOL_SIZE);
        pool = vmm_map_wc(pool_phys, POOL_SIZE);
        if (!pool) { fail("cannot map the copy engines' memory"); return -1; }
    }
    if (smmu_setup()) { fail(smmu_note[0] ? smmu_note : "no SMMU entry for the WLAN"); return -1; }
    memset(eps, 0, sizeof eps);
    eps[0].svc = SVC_RSVD_CTRL; eps[0].ul = 0; eps[0].dl = 2;
    if (ce_init()) { fail("copy engine setup failed (memory)"); return -1; }
    kprintf("ath: copy engines up; waiting for HTC ready\n");
    stage = A_HTC_READY;
    return 0;
}

void qcom_ath_poll(void)
{
    if (stage == A_OFF || stage == A_FAILED) return;
    ce_poll();
}

int qcom_ath_scan(void)
{
    if (stage != A_UP) return -1;
    if (!vdev_up) { send_chan_list(); send_vdev_create(); vdev_up = 1; }
    if (!scanning) send_start_scan();
    return 0;
}

int qcom_ath_state(void) { return stage == A_UP ? (scanning ? 2 : 1) : stage == A_FAILED ? -1 : 0; }

/* The networks, strongest first, one line per name: "net <dBm> <secure> <ssid>". */
size_t qcom_ath_nets(char *buf, size_t len)
{
    size_t n = 0;
    int done[MAX_BSS] = { 0 };
    for (;;) {
        int best = -1;
        for (int i = 0; i < nbss; i++) {
            if (done[i] || !bsses[i].ssid[0]) continue;
            int dup = 0;
            for (int j = 0; j < nbss; j++) if (done[j] == 2 && !strcmp(bsses[j].ssid, bsses[i].ssid)) dup = 1;
            if (dup) { done[i] = 1; continue; }
            if (best < 0 || bsses[i].rssi > bsses[best].rssi) best = i;
        }
        if (best < 0 || n >= len) break;
        done[best] = 2;
        n += (size_t)ksnprintf(buf + n, len - n, "net %d %d %s\n", bsses[best].rssi, bsses[best].secure ? 1 : 0, bsses[best].ssid);
    }
    return n;
}

size_t qcom_ath_report(char *buf, size_t len)
{
    size_t n = (size_t)ksnprintf(buf, len, "radio: %s%s%s\n", stage_names[stage], stage == A_FAILED ? ": " : "", stage == A_FAILED ? why : "");
    if (smmu_note[0] && n < len) n += (size_t)ksnprintf(buf + n, len - n, "  smmu: %s\n", smmu_note);
    if (pmic_ok && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  rails (EN_CTL/STATUS1/V): L8 %02x/%02x/%02x L16 %02x/%02x/%02x L17 %02x/%02x/%02x L23 %02x/%02x/%02x\n",
                               pmic_l[0][0], pmic_l[0][1], pmic_l[0][2], pmic_l[1][0], pmic_l[1][1], pmic_l[1][2],
                               pmic_l[2][0], pmic_l[2][1], pmic_l[2][2], pmic_l[3][0], pmic_l[3][1], pmic_l[3][2]);
    if (stage != A_OFF && n < len) {
        n += (size_t)ksnprintf(buf + n, len - n, "  ce rx:");
        for (int i = 0; i < CE_COUNT && n < len; i++) if (pipes[i].dst_n) n += (size_t)ksnprintf(buf + n, len - n, " %d:%u", i, pipes[i].rx_count);
        if (n < len) n += (size_t)ksnprintf(buf + n, len - n, "; tx: 0:%u 3:%u; htc %u msgs; wmi %u events (%u unknown, last %x), credits %d, queued %d\n",
                                           pipes[0].tx_count, pipes[3].tx_count, htc_rx, wmi_events, wmi_unknown, last_unknown,
                                           wmi_ep >= 0 ? eps[wmi_ep].credits : -1, wq_len);
    }
    if (stage == A_UP && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  MAC %02x:%02x:%02x:%02x:%02x:%02x, %d scans%s, %u frames, %d networks\n",
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], scans, scanning ? " (scanning)" : "", mgmt_rx, nbss);
    return n;
}
