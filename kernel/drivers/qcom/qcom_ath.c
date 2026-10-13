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
#include "drivers/qcom/qcom_ipc.h"
#include "drivers/qcom/qcom_scm.h"
#include "drivers/qcom/qcom_smem.h"
#include "asm/timer.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "crypto.h"
#include "random.h"
#include "spinlock.h"
#include "net/net.h"

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
static int mac_made;
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
            if (i == 0 || i == 3 || i == 4 || i == 7)
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
static uint32_t htc_rx, htc_unknown, credits_back;

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
    WMI_INIT = 0x1, WMI_START_SCAN = 0x3001, WMI_SCAN_CHAN_LIST = 0x3003, WMI_PDEV_SET_REGDOMAIN = 0x4001,
    WMI_PDEV_SET_PARAM = 0x4003, WMI_VDEV_CREATE = 0x5001,
    EV_SERVICE_READY = 0x1, EV_READY = 0x2, EV_SCAN = 0x3001, EV_CHAN_INFO = 0x4002, EV_MGMT_RX = 0x7001,
};
enum {
    TAG_ARRAY_UINT32 = 0x10, TAG_ARRAY_BYTE = 0x11, TAG_ARRAY_STRUCT = 0x12, TAG_ARRAY_FIXED_STRUCT = 0x13,
    TAG_SERVICE_READY = 0x20, TAG_HAL_REG_CAPS = 0x21, TAG_HOST_MEM_REQ = 0x22, TAG_READY = 0x23, TAG_SCAN_EVENT = 0x24, TAG_CHAN_INFO = 0x26, TAG_MGMT_RX_HDR = 0x2c,
    TAG_INIT_CMD = 0x4a, TAG_RESOURCE_CONFIG = 0x4b, TAG_HOST_MEMORY_CHUNK = 0x4c, TAG_START_SCAN = 0x4d,
    TAG_SCAN_CHAN_LIST = 0x4f, TAG_CHANNEL = 0x50, TAG_PDEV_SET_REGDOMAIN = 0x51, TAG_PDEV_SET_PARAM = 0x52, TAG_VDEV_CREATE = 0x56,
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

static uint32_t eeprom_rd, hw_rf_chains, hw_vht_cap, hw_vht_mcs;

static void send_pdev_param(uint32_t param, uint32_t value)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PDEV_SET_PARAM, 3 * 4);
    put32(c, 1, param); put32(c, 2, value);
    wmi_send(WMI_PDEV_SET_PARAM, t.b, t.n);
}

/* What ath10k_start sends before mac80211 may scan: its pdev parameters
 * (WMI-TLV's numbering), then the regulatory domain the board data names
 * (conformance limits: FCC's tables). */
static void send_pdev_setup(void)
{
    send_pdev_param(34, 1);                                 /* pmf_qos */
    send_pdev_param(10, 1);                                 /* dynamic_bw */
    send_pdev_param(44, 1);                                 /* idle_ps_config */
    send_pdev_param(35, 0);                                 /* arp_ac_override */
    send_pdev_param(37, 1);                                 /* ani_enable */
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PDEV_SET_REGDOMAIN, 6 * 4);
    put32(c, 1, eeprom_rd); put32(c, 2, eeprom_rd); put32(c, 3, eeprom_rd);
    put32(c, 4, 0x10); put32(c, 5, 0x10);
    wmi_send(WMI_PDEV_SET_REGDOMAIN, t.b, t.n);
}

/* ---- HTT setup: what ath10k_htt_setup sends once WMI is ready ------------------------------- */

enum { HTT_H2T_VERSION_REQ = 0, HTT_H2T_RX_RING_CFG = 2, HTT_H2T_AGGR_CFG = 5, HTT_H2T_FRAG_DESC_BANK_CFG = 6 };
enum { HTT_T2H_VERSION_CONF = 0 };
#define HTT_TX_DESCS    1056        /* TARGET_TLV_NUM_MSDU_DESC */
#define HTT_FRAG_DESC   72          /* htt_msdu_ext_desc_64 */
#define HTT_RX_RING     256
#define HTT_RX_BUF      2048

static int htt_stage;               /* 0 none, 1 version asked, 2 configured */
static uint8_t htt_major, htt_minor;
static uint32_t htt_rx_msgs, htt_types;
static uint64_t frag_phys, rxring_phys, rxidx_phys, rxbufs_phys;

static void put16le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }

static uint64_t htt_alloc(size_t size)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t ph = pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!ph) return 0;
    memset(P2V(ph), 0, pages * PAGE_SIZE);
    qcom_dma_clean(P2V(ph), pages * PAGE_SIZE);
    return ph;
}

static void htt_version_req(void)
{
    uint8_t m[4] = { HTT_H2T_VERSION_REQ };
    if (htt_ep < 0 || htc_send(htt_ep, m, sizeof m)) { kprintf("ath: HTT version request not sent\n"); return; }
    htt_stage = 1;
}

/* The target's answer came: the transmit descriptors' memory, the receive
 * ring (buffers the target fills with frames for the host), aggregation. */
static void htt_configure(void)
{
    if (!frag_phys) frag_phys = htt_alloc(HTT_TX_DESCS * HTT_FRAG_DESC);
    if (!rxring_phys) rxring_phys = htt_alloc(HTT_RX_RING * 8);
    if (!rxidx_phys) rxidx_phys = htt_alloc(4);
    if (!rxbufs_phys) rxbufs_phys = htt_alloc((size_t)(HTT_RX_RING - 1) * HTT_RX_BUF);
    if (!frag_phys || !rxring_phys || !rxidx_phys || !rxbufs_phys) { kprintf("ath: no memory for HTT\n"); return; }

    uint8_t f[64] = { HTT_H2T_FRAG_DESC_BANK_CFG, 0, 1, HTT_FRAG_DESC };
    put64le(f + 4, frag_phys);
    put16le(f + 36, 0); put16le(f + 38, HTT_TX_DESCS - 1);
    htc_send(htt_ep, f, sizeof f);

    /* the ring: the buffers' addresses; how many are filled, where the target reads it */
    uint64_t *ring = P2V(rxring_phys);
    for (int i = 0; i < HTT_RX_RING - 1; i++) ring[i] = rxbufs_phys + (uint64_t)i * HTT_RX_BUF;
    qcom_dma_clean(ring, HTT_RX_RING * 8);
    volatile uint32_t *idx = P2V(rxidx_phys);
    *idx = HTT_RX_RING - 1;
    qcom_dma_clean((void *)idx, 4);

    uint8_t r[48] = { HTT_H2T_RX_RING_CFG, 1 };
    put64le(r + 4, rxidx_phys);
    put64le(r + 12, rxring_phys);
    put16le(r + 20, HTT_RX_RING);
    put16le(r + 22, HTT_RX_BUF);
    put16le(r + 24, 0xffff);                                /* every part of the descriptor */
    put16le(r + 26, 0);
    static const uint16_t offs[10] = { 74, 90, 27, 37, 4, 26, 7, 12, 1, 2 };   /* wcn3990's rx descriptor, in words */
    for (int i = 0; i < 10; i++) put16le(r + 28 + 2 * i, offs[i]);
    htc_send(htt_ep, r, sizeof r);

    uint8_t a[4] = { HTT_H2T_AGGR_CFG, 64, 3, 0 };          /* A-MPDU 64, A-MSDU 3 */
    htc_send(htt_ep, a, sizeof a);
    htt_stage = 2;
    kprintf("ath: HTT %u.%u configured\n", htt_major, htt_minor);
}

/* ---- HTT receive: data frames ----------------------------------------------------------- */
/* This firmware reorders on its own (RX_IN_ORD_PADDR_IND): it says which
 * ring buffers it filled, by physical address, in order. Each buffer is
 * the chip's 360-byte receive descriptor, then the frame: native Wi-Fi
 * (the 802.11 header without QoS, then LLC/SNAP), after 0-7 bytes of pad.
 * "Offloaded" frames (ones the firmware handled itself) carry a 7-byte
 * header instead of the descriptor. Each buffer goes back into the ring
 * once it is read. */

enum { HTT_T2H_RX_IN_ORD_PADDR_IND = 0x12, HTT_T2H_TX_COMPL_IND = 7 };
static uint32_t htt_tx_n, htt_tx_compl;
#define RXD_SIZE        360
#define RXD_ATTENTION   4           /* bit 31: MSDU done; 29 decrypt error, 30 FCS error */
#define RXD_MSDU_START1 36          /* bits 8-9: decap format */
#define RXD_MSDU_END1   80          /* wcn3990: bits 10-12: L3 header pad */

static uint32_t rx_alloc = HTT_RX_RING - 1;     /* the next ring slot to give back */
static uint32_t rx_frames, rx_bad, rx_eapol, rx_ip, rx_arp, rx_other, rx_offload, rx_lost, rx_amsdu, rx_qos;
static uint16_t rx_last_type;

static void wlan_frame(const uint8_t *f, size_t n, int decrypted);
static void deliver_llc(const uint8_t *da, const uint8_t *sa, const uint8_t *p, size_t n);

static void rx_refill(uint64_t paddr)
{
    volatile uint64_t *ring = P2V(rxring_phys);
    ring[rx_alloc & (HTT_RX_RING - 1)] = paddr;
    qcom_dma_clean((const void *)&ring[rx_alloc & (HTT_RX_RING - 1)], 8);
    rx_alloc++;
    volatile uint32_t *idx = P2V(rxidx_phys);
    __asm__ volatile("dsb sy" ::: "memory");
    *idx = rx_alloc & (HTT_RX_RING - 1);
    qcom_dma_clean((const void *)idx, 4);
}

static void htt_rx_in_ord(const uint8_t *p, size_t n)
{
    if (n < 8) return;
    int offload = (p[1] & 0x20) != 0;
    unsigned count = (unsigned)(p[6] | p[7] << 8);
    const uint8_t *d = p + 8;
    for (unsigned i = 0; i < count && d + 12 <= p + n; i++, d += 12) {
        uint64_t pa = 0;
        for (int k = 0; k < 8; k++) pa |= (uint64_t)d[k] << 8 * k;
        unsigned len = (unsigned)(d[8] | d[9] << 8);
        if (pa < rxbufs_phys || pa >= rxbufs_phys + (uint64_t)(HTT_RX_RING - 1) * HTT_RX_BUF || (pa - rxbufs_phys) % HTT_RX_BUF) { rx_lost++; continue; }
        uint8_t *b = P2V(pa);
        qcom_dma_clean(b, HTT_RX_BUF);                      /* what the radio wrote, not stale cache lines */
        rx_frames++;
        if (offload) {
            unsigned ol = (unsigned)(b[0] | b[1] << 8);
            rx_offload++;
            if (7 + ol <= HTT_RX_BUF) wlan_frame(b + 7, ol, 1);
        } else {
            uint32_t att, s1, e1;
            memcpy(&att, b + RXD_ATTENTION, 4); memcpy(&s1, b + RXD_MSDU_START1, 4); memcpy(&e1, b + RXD_MSDU_END1, 4);
            unsigned pad = (e1 >> 10) & 7;
            if (!(att & 1u << 31) || (att & (3u << 29)) || ((s1 >> 8) & 3) != 1 || RXD_SIZE + pad + len > HTT_RX_BUF) rx_bad++;
            else wlan_frame(b + RXD_SIZE + pad, len, 0);
        }
        rx_refill(pa);
    }
}

/* Other target-to-host messages, handled past the data path: the ones that
 * carry peer ids, security notifications, fragmented frames and the IPA
 * "offload" delivery. Counted (and, for offload delivery, the inner MSDUs
 * are fed up like ordinary frames); the rest of the protocol is noted. */
enum { HTT_T2H_PEER_MAP = 3, HTT_T2H_PEER_UNMAP = 4, HTT_T2H_SEC_IND = 11, HTT_T2H_RX_FRAG_IND = 10,
       HTT_T2H_RX_OFFLOAD_DELIVER_IND = 0x11 };
static uint32_t htt_peer_evts, htt_sec_ind_n, htt_frag_n, htt_offload_n, htt_misc;

/* RX_OFFLOAD_DELIVER_IND: a count, then per MSDU a short header (msdu_len,
 * peer_id, vdev_id, tid, fw_desc) and the 802.3 frame (DA, SA, length). */
static void htt_rx_offload(const uint8_t *p, size_t n)
{
    unsigned count = n >= 2 ? (unsigned)(p[1] | (n >= 3 ? p[2] << 8 : 0)) : 0;
    const uint8_t *d = p + 4;
    for (unsigned i = 0; i < count && d + 7 <= p + n; i++) {
        unsigned len = (unsigned)(d[0] | d[1] << 8);
        const uint8_t *e = d + 7;                           /* the Ethernet frame: DA(6) SA(6) len(2) then LLC */
        if (e + len > p + n || len < 14) break;
        rx_offload++;
        deliver_llc(e, e + 6, e + 14, len - 14);
        d = e + ((len + 3) & ~3u);
    }
}

static void htt_rx_msg(const uint8_t *p, size_t n)
{
    if (!n) return;
    htt_rx_msgs++;
    if (p[0] < 32) htt_types |= 1u << p[0];
    switch (p[0]) {
    case HTT_T2H_VERSION_CONF:
        if (n >= 3 && htt_stage == 1) { htt_minor = p[1]; htt_major = p[2]; htt_configure(); }
        break;
    case HTT_T2H_RX_IN_ORD_PADDR_IND:
        if (htt_stage == 2) htt_rx_in_ord(p, n);
        break;
    case HTT_T2H_TX_COMPL_IND:
        if (n >= 3) htt_tx_compl += p[2];                   /* num_msdus done */
        break;
    case HTT_T2H_RX_OFFLOAD_DELIVER_IND:
        htt_offload_n++;
        if (htt_stage == 2) htt_rx_offload(p, n);
        break;
    case HTT_T2H_PEER_MAP: case HTT_T2H_PEER_UNMAP: htt_peer_evts++; break;
    case HTT_T2H_SEC_IND: htt_sec_ind_n++; break;
    case HTT_T2H_RX_FRAG_IND: htt_frag_n++; break;          /* fragmented frames: not reassembled */
    default: htt_misc++; break;
    }
}

/* ---- HTT transmit ------------------------------------------------------------------------- */
/* Only EAPOL in the 4-way handshake goes out this way (ath10k_htt_tx_64):
 * HTT_H2T_MSG_TYPE_TX_FRM carries the data descriptor; the 802.11 frame
 * stays in a buffer the radio reads by reference through a fragment
 * descriptor in the bank set up at HTT configure time (one per msdu id). */
enum { HTT_H2T_TX_FRM = 1 };
#define HTT_TX_SLOTS 32
#define HTT_TX_BUF   2048

static uint8_t *htt_tx_buf[HTT_TX_SLOTS];
static uint64_t htt_tx_phys[HTT_TX_SLOTS];
static int htt_tx_next;
static uint32_t htt_tx_busy;            /* refused: every slot still in flight */

/* crypt: 0 sends the frame as it is (EAPOL before the keys are in), 1
 * lets the radio encrypt it with the installed key. */
static int htt_tx_data(const uint8_t *frame, size_t len, int crypt)
{
    if (htt_ep < 0 || htt_stage != 2 || !frag_phys || len > HTT_TX_BUF) return -1;
    if (!htt_tx_buf[0]) {
        uint64_t ph = htt_alloc((size_t)HTT_TX_SLOTS * HTT_TX_BUF);
        if (!ph) return -1;
        for (int i = 0; i < HTT_TX_SLOTS; i++) { htt_tx_phys[i] = ph + (uint64_t)i * HTT_TX_BUF; htt_tx_buf[i] = P2V(htt_tx_phys[i]); }
    }
    if (htt_tx_n - htt_tx_compl >= HTT_TX_SLOTS) { htt_tx_busy++; return -1; }
    int id = htt_tx_next++ % HTT_TX_SLOTS;
    memcpy(htt_tx_buf[id], frame, len);
    qcom_dma_clean(htt_tx_buf[id], len);

    /* the fragment descriptor for this id: the one fragment, then a zero
     * terminator (htt_msdu_ext_desc_64: frags[] start 24 bytes in) */
    uint8_t *ext = P2V(frag_phys + (uint64_t)HTT_FRAG_DESC * id);
    memset(ext, 0, HTT_FRAG_DESC);
    uint64_t fa = htt_tx_phys[id];
    put32(ext + 24, 0, (uint32_t)fa);                       /* frags[0] paddr_lo */
    put16le(ext + 24 + 4, (uint32_t)(fa >> 32));            /* paddr_hi */
    put16le(ext + 24 + 6, (uint32_t)len);                   /* len */
    qcom_dma_clean(ext, HTT_FRAG_DESC);
    __asm__ volatile("dsb sy" ::: "memory");

    /* the command: htt_cmd_hdr (1), htt_data_tx_desc_64 (19), a prefetch */
    uint8_t c[1 + 19 + 64];
    size_t pf = len < 64 ? len : 64;
    c[0] = HTT_H2T_TX_FRM;
    c[1] = crypt ? 0x23 : 0x27;                             /* mac hdr present, no aggr, [no encrypt], native wifi (1 << 5) */
    put16le(c + 2, 0x0400);                                 /* vdev 0, ext tid 16 (non-QoS) */
    put16le(c + 4, (uint32_t)len);
    put16le(c + 6, (uint32_t)id);
    put64le(c + 8, frag_phys + (uint64_t)HTT_FRAG_DESC * id);
    put32(c + 16, 0, 0xffff);                               /* peer id: let the firmware find it */
    memcpy(c + 20, frame, pf);
    if (htc_send(htt_ep, c, 20 + pf)) return -1;
    htt_tx_n++;
    return 0;
}

/* ---- scanning ----------------------------------------------------------------------------------- */

#define MAX_BSS 48
/* secure: 0 open, 1 WPA (not supported), 2 RSN. What joining needs: the
 * capability field, beacon interval and DTIM period, the rates offered,
 * and from the RSN element the group cipher and whether PSK and CCMP are
 * offered (and protected management frames required). */
static struct bss {
    uint8_t bssid[6]; char ssid[33]; int rssi, freq, secure; uint64_t seen;
    uint16_t cap, bcn_int; uint8_t dtim, nrates, rates[16];
    uint8_t group[4]; uint8_t psk, ccmp, mfp_req;
    uint8_t akm_psk256, akm_sae, mfp_cap;       /* the RSN's AKMs and PMF bits */
    uint8_t has_ht; uint16_t ht_cap; uint8_t ampdu_param; uint8_t ht_mcs1;
    uint8_t has_vht, vht_width, vht_cf1, vht_cf2;
    uint32_t vht_cap; uint16_t vht_rx_mcs;
} bsses[MAX_BSS];
static int nbss;
static int vdev_up, scanning, scans;
static uint32_t mgmt_rx;
static uint32_t chinfo_n, clear_max, last_freq, last_cycles;
static int32_t nf_min = 1000, nf_max = -1000;

static const uint16_t chans_2g[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472 };
static const uint16_t chans_5g[] = { 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580,
                                     5600, 5620, 5640, 5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825 };
#define N2G (sizeof chans_2g / sizeof chans_2g[0])
#define N5G (sizeof chans_5g / sizeof chans_5g[0])
static int chan_to_freq(int chan)
{
    if (chan >= 1 && chan <= 13) return 2407 + 5 * chan;
    if (chan == 14) return 2484;
    if (chan >= 36 && chan <= 177) return 5000 + 5 * chan;
    if (chan >= 2400) return chan;
    return chan;
}

static int is2g(int freq) { return freq < 3000; }

static int vht80_center(int freq)
{
    if (freq >= 5180 && freq <= 5240) return 5210;
    if (freq >= 5260 && freq <= 5320) return 5290;
    if (freq >= 5500 && freq <= 5560) return 5530;
    if (freq >= 5580 && freq <= 5640) return 5610;
    if (freq >= 5660 && freq <= 5720) return 5690;
    if (freq >= 5745 && freq <= 5805) return 5775;
    return freq;
}

static int bss_score(const struct bss *b)
{
    int score = b->rssi;
    /* Strongly prefer 5 GHz when available and usable (signal >= -80 dBm).
     * Dual-band APs have higher RSSI on 2.4 GHz due to physics, but 5 GHz VHT80
     * provides vastly superior bandwidth (up to 867 Mbps vs 65 Mbps). */
    if (!is2g(b->freq) && b->rssi >= -80) {
        score += 30;
        if (b->has_vht) score += 10;
    }
    return score;
}

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
        if (!is2) flags |= 1u << 12;                        /* VHT allowed */
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

/* The RSN element: version, group cipher, pairwise ciphers, AKMs (2 PSK,
 * 6 PSK-SHA256, 8 SAE), capabilities (MFP capable/required). */
static void rsn_parse(struct bss *b, const uint8_t *v, size_t l)
{
    static const uint8_t oui[3] = { 0x00, 0x0f, 0xac };
    if (l < 8 || v[0] != 1 || v[1]) return;
    memcpy(b->group, v + 2, 4);
    size_t o = 6;
    unsigned np = (unsigned)(v[o] | v[o + 1] << 8);
    o += 2;
    for (unsigned i = 0; i < np && o + 4 <= l; i++, o += 4) if (!memcmp(v + o, oui, 3) && v[o + 3] == 4) b->ccmp = 1;
    if (o + 2 > l) return;
    unsigned na = (unsigned)(v[o] | v[o + 1] << 8);
    o += 2;
    for (unsigned i = 0; i < na && o + 4 <= l; i++, o += 4)
        if (!memcmp(v + o, oui, 3)) {
            if (v[o + 3] == 2) b->psk = 1;
            else if (v[o + 3] == 6) b->akm_psk256 = 1;
            else if (v[o + 3] == 8) b->akm_sae = 1;
        }
    if (o + 2 <= l) { uint16_t caps = (uint16_t)(v[o] | v[o + 1] << 8); if (caps & 0x80) b->mfp_cap = 1; if (caps & 0x40) b->mfp_req = 1; }
}

static void join_saw(struct bss *b);

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
    struct bss nb = { 0 };
    nb.cap = cap;
    nb.bcn_int = (uint16_t)(f[32] | f[33] << 8);
    nb.dtim = 1;
    for (size_t o = 36; o + 2 <= n; ) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > n) break;
        const uint8_t *v = f + o + 2;
        if (id == 0 && l <= 32) { memcpy(ssid, v, l); ssid[l] = 0; }
        else if ((id == 3 || id == 61) && l >= 1) ds = v[0]; /* DS param or HT op: channel */
        else if ((id == 1 || id == 50) && l)                /* supported, extended rates */
            for (int i = 0; i < l && nb.nrates < sizeof nb.rates; i++) nb.rates[nb.nrates++] = v[i];
        else if (id == 5 && l >= 2 && v[1]) nb.dtim = v[1];
        else if (id == 45 && l >= 2) {                       /* HT capabilities */
            nb.has_ht = 1;
            nb.ht_cap = (uint16_t)(v[0] | v[1] << 8);
            if (l >= 3) nb.ampdu_param = v[2];
            if (l >= 5) nb.ht_mcs1 = v[4];                  /* MCS 8..15 bitmask */
        }
        else if (id == 191 && l >= 12) {                     /* VHT capabilities */
            nb.has_vht = 1;
            nb.vht_cap = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
            nb.vht_rx_mcs = (uint16_t)((uint32_t)v[4] | ((uint32_t)v[5] << 8));
        }
        else if (id == 192 && l >= 3) {                      /* VHT operation */
            nb.has_vht = 1;
            nb.vht_width = v[0];
            nb.vht_cf1 = v[1];
            nb.vht_cf2 = v[2];
        }
        else if (id == 48) { secure = 2; rsn_parse(&nb, v, l); }     /* RSN: WPA2/WPA3 */
        else if (id == 221 && l >= 4 && v[0] == 0 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 1 && secure < 2) secure = 1;
        o += 2 + (size_t)l;
    }
    if (ds) freq = chan_to_freq(ds);
    else if (freq < 1000) freq = chan_to_freq(freq);
    for (size_t i = 0; ssid[i]; i++) if ((unsigned char)ssid[i] < 32) ssid[i] = '?';
    struct bss *b = NULL;
    for (int i = 0; i < nbss; i++) if (!memcmp(bsses[i].bssid, bssid, 6)) b = &bsses[i];
    if (!b && nbss < MAX_BSS) b = &bsses[nbss++];
    if (!b) return;
    *b = nb;
    memcpy(b->bssid, bssid, 6);
    memcpy(b->ssid, ssid, sizeof ssid);
    b->rssi = rssi; b->freq = freq; b->secure = secure; b->seen = timer_ms();
    join_saw(b);
}

/* ---- joining a network ------------------------------------------------------------------ */
/* What mac80211 and ath10k do for a station, in order: the vdev started
 * on the network's channel (VDEV_START, wait for the response), a peer
 * for the access point, open authentication and the association request
 * as management frames (WMI's MGMT_TX_SEND: the frame by reference),
 * then PEER_ASSOC with the rates both sides have and VDEV_UP with the
 * association id. Legacy rates only for now (11a/11g, no HT). A network
 * with RSN goes on to the 4-way handshake; an open one is authorized. */

enum { WMI_VDEV_START = 0x5003, WMI_VDEV_UP = 0x5005, WMI_VDEV_STOP = 0x5006, WMI_VDEV_DOWN = 0x5007, WMI_VDEV_INSTALL_KEY = 0x5009,
       WMI_PEER_CREATE = 0x6001, WMI_PEER_DELETE = 0x6002, WMI_PEER_SET_PARAM = 0x6004,
       WMI_PEER_ASSOC = 0x6005, WMI_MGMT_TX_SEND = 0x7008, WMI_STA_POWERSAVE_MODE = 0x9001,
       EV_VDEV_START_RESP = 0x5001, EV_PEER_KICKOUT = 0x6001, EV_MGMT_TX_DONE = 0x7006, EV_MGMT_TX_BUNDLE = 0x7007 };
enum { TAG_VDEV_START = 0x58, TAG_VDEV_UP = 0x5c, TAG_VDEV_STOP = 0x5d, TAG_VDEV_DOWN = 0x5e, TAG_VDEV_INSTALL_KEY = 0x60,
       TAG_PEER_CREATE = 0x61, TAG_PEER_DELETE = 0x62, TAG_PEER_SET_PARAM = 0x64,
       TAG_PEER_ASSOC = 0x65, TAG_VHT_RATE_SET = 0x66, TAG_STA_PS_MODE = 0x6c, TAG_MGMT_TX = 0x1a6, TAG_VDEV_START_RESP = 0x28 };
enum { WMI_CIPHER_AES_CCM = 4, WMI_CIPHER_AES_CMAC = 7, WMI_KEY_PAIRWISE = 0, WMI_KEY_GROUP = 1 };
enum { PEER_QOS = 0x2, PEER_NEED_PTK_4_WAY = 0x4, PEER_NEED_GTK_2_WAY = 0x10,
       PEER_HT = 0x1000, PEER_STBC = 0x8000, PEER_LDPC = 0x10000, PEER_PMF = 0x08000000 };
enum { J_IDLE, J_FIND, J_START, J_SAE_COMMIT, J_SAE_CONFIRM, J_AUTH, J_ASSOC, J_KEYS, J_UP, J_FAILED };
static const char *const join_names[] = { "not connected", "looking for the network", "tuning to its channel", "the SAE commit",
    "the SAE confirm", "authenticating", "associating", "the 4-way handshake", "connected", "failed" };

static int jstate;
static char jssid[33], jpass[64], jwhy[96];
static struct bss jb;                       /* the network being joined */
static uint16_t jaid, jcap, mgmt_seq;
static uint64_t jsent;
static int jtries, jakm, jmfp;              /* the chosen AKM (2, 6, 8) and whether to protect management frames */
static uint32_t mgmt_sent, mgmt_done;
static int jauto, reconn_tries;             /* reconnect when an established link is lost */
static uint64_t reconn_at;

/* The 4-way handshake: the PMK (the passphrase, or SAE), the PTK from the
 * nonces, the GTK (and, with PMF, the IGTK) the access point sends. */
static uint8_t pmk[32], ptk[48];            /* PTK: KCK 0..15, KEK 16..31, TK 32..47 */
static uint8_t anonce[32], snonce[32], replay[8], rx_replay[8];
static uint8_t gtk[32], gtk_rsc[8], igtk[16];
static int gtk_len, gtk_id, igtk_id, hs;    /* hs: 0 none, 1 message 2 sent, 2 keys in */
static uint32_t eapol_rx_n, eapol_tx_n, eapol_replays, group_rekeys;

/* SAE (WPA3-Personal): the password element, our and the peer's commit, the
 * KCK and the PMK the dragonfly exchange yields. */
static uint8_t sae_pwe[64], sae_rand[32], sae_mask[32], sae_scalar[32], sae_element[64];
static uint8_t sae_peer_scalar[32], sae_peer_element[64], sae_kck[32], sae_token[256];
static int sae_token_len, sae_confirmed;

#define MGMT_BUFS 4
static uint8_t *mgmt_buf[MGMT_BUFS];
static uint64_t mgmt_phys[MGMT_BUFS];
static int mgmt_next;

static void wlan0_up(void), wlan0_down(void), wlan0_send(void);
static void wlan0_in(const uint8_t *dst, const uint8_t *src, uint16_t type, const uint8_t *p, size_t n);

static void join_fail(const char *fmt, unsigned a)
{
    wlan0_down();
    ksnprintf(jwhy, sizeof jwhy, fmt, a);
    kprintf("ath: join: %s: %s\n", join_names[jstate], jwhy);
    jstate = J_FAILED;
}

static void put_mac(void *p, const uint8_t *m) { memset(p, 0, 8); memcpy(p, m, 6); }

/* A management frame out: the frame stays in memory the radio reads (by
 * reference), its first 64 bytes go with the command as well. */
static int mgmt_tx(const uint8_t *frame, size_t len)
{
    if (!mgmt_buf[0])
        for (int i = 0; i < MGMT_BUFS; i++)
            if (!(mgmt_buf[i] = dma_alloc(512, &mgmt_phys[i]))) return -1;
    if (len > 512) return -1;
    int i = mgmt_next++ % MGMT_BUFS;
    memcpy(mgmt_buf[i], frame, len);
    __asm__ volatile("dsb sy" ::: "memory");
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_MGMT_TX, 28);
    put32(c, 0, 0);                                         /* vdev */
    put32(c, 1, (uint32_t)i);                               /* descriptor id */
    put32(c, 2, 0);                                         /* channel: the vdev's */
    put32(c, 3, (uint32_t)mgmt_phys[i]); put32(c, 4, (uint32_t)(mgmt_phys[i] >> 32));
    put32(c, 5, (uint32_t)len);                             /* frame length */
    size_t blen = ((len < 64 ? len : 64) + 3) & ~(size_t)3;
    put32(c, 6, (uint32_t)blen);
    uint8_t *b = tlv_put(&t, TAG_ARRAY_BYTE, blen);
    memcpy(b, frame, len < blen ? len : blen);
    mgmt_sent++;
    return wmi_send(WMI_MGMT_TX_SEND, t.b, t.n);
}

/* The 24-byte header of a frame to the access point. */
static size_t mgmt_hdr(uint8_t *f, uint8_t subtype)
{
    f[0] = subtype; f[1] = 0; f[2] = f[3] = 0;
    memcpy(f + 4, jb.bssid, 6); memcpy(f + 10, mac, 6); memcpy(f + 16, jb.bssid, 6);
    uint16_t sc = (uint16_t)(mgmt_seq++ << 4);
    f[22] = (uint8_t)sc; f[23] = (uint8_t)(sc >> 8);
    return 24;
}

static void send_vdev_start(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_VDEV_START, 72);
    put32(c, 0, 0);                                         /* vdev */
    put32(c, 2, jb.bcn_int ? jb.bcn_int : 100);
    put32(c, 3, jb.dtim);
    uint32_t *ch = tlv_put(&t, TAG_CHANNEL, 24);
    put32(ch, 0, (uint32_t)jb.freq);
    int use_vht = !is2g(jb.freq) && jb.has_vht;
    uint32_t cf1 = (uint32_t)jb.freq;
    if (use_vht) {
        cf1 = (uint32_t)(jb.vht_cf1 ? 5000 + 5 * (int)jb.vht_cf1 : vht80_center(jb.freq));
    }
    put32(ch, 1, cf1);
    put32(ch, 2, 0);
    uint32_t mode;
    if (use_vht) {
        mode = 10u /* MODE_11AC_VHT80 */ | (1u << 11) /* ALLOW_HT */ | (1u << 12) /* ALLOW_VHT */;
    } else if (is2g(jb.freq)) {
        mode = jb.has_ht ? 5u /* MODE_11NG_HT20 */ : 1u /* MODE_11G */;
        if (jb.has_ht) mode |= 1u << 11;
    } else {
        mode = jb.has_ht ? 4u /* MODE_11NA_HT20 */ : 0u /* MODE_11A */;
        if (jb.has_ht) mode |= 1u << 11;
    }
    put32(ch, 3, mode);
    put32(ch, 4, 0 | 40u << 8 | 40u << 16);
    put32(ch, 5, 0 | 40u << 8);
    tlv_put(&t, TAG_ARRAY_STRUCT, 0);                       /* no NoA descriptors */
    kprintf("ath: vdev_start: freq %u cf1 %u mode %u\n", (unsigned)jb.freq, (unsigned)cf1, (unsigned)mode);
    wmi_send(WMI_VDEV_START, t.b, t.n);
}

static void send_peer_create(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PEER_CREATE, 16);
    put32(c, 0, 0);
    put_mac(c + 1, jb.bssid);
    put32(c, 3, 0);                                         /* default peer */
    wmi_send(WMI_PEER_CREATE, t.b, t.n);
}

static void send_peer_param(uint32_t param, uint32_t value)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PEER_SET_PARAM, 20);
    put32(c, 0, 0);
    put_mac(c + 1, jb.bssid);
    put32(c, 3, param); put32(c, 4, value);
    wmi_send(WMI_PEER_SET_PARAM, t.b, t.n);
}

/* Tearing the connection down (for a reconnect): the vdev down, the access
 * point's peer deleted, the vdev stopped, so a fresh VDEV_START can run. */
static void send_vdev_down(void)
{
    static struct tb t;
    t.n = 0;
    tlv_put(&t, TAG_VDEV_DOWN, 4);                          /* vdev 0 */
    wmi_send(WMI_VDEV_DOWN, t.b, t.n);
}
static void send_vdev_stop(void)
{
    static struct tb t;
    t.n = 0;
    tlv_put(&t, TAG_VDEV_STOP, 4);
    wmi_send(WMI_VDEV_STOP, t.b, t.n);
}
static void send_peer_delete(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PEER_DELETE, 12);
    put32(c, 0, 0);
    put_mac(c + 1, jb.bssid);
    wmi_send(WMI_PEER_DELETE, t.b, t.n);
}

static void send_auth(void)
{
    uint8_t f[30];
    size_t n = mgmt_hdr(f, 0xb0);
    memset(f + n, 0, 6);
    f[n + 2] = 1;                                           /* open system, sequence 1, status 0 */
    mgmt_tx(f, n + 6);
    jsent = timer_ms();
}

/* The station's RSN element: the group cipher the network named, CCMP for
 * the pair, the chosen AKM, and (when protecting management frames) the MFP
 * capable bit, a zero PMKID list and the BIP group-management cipher. Used
 * both in the association request and as the key data of handshake message 2. */
static size_t build_rsn_ie(uint8_t *p)
{
    size_t n = 0;
    p[n++] = 48; size_t lp = n++;                           /* element id, length (filled below) */
    p[n++] = 1; p[n++] = 0;                                 /* version 1 */
    memcpy(p + n, jb.group, 4); n += 4;                     /* group data cipher */
    p[n++] = 1; p[n++] = 0;                                 /* one pairwise cipher: CCMP */
    memcpy(p + n, "\x00\x0f\xac\x04", 4); n += 4;
    p[n++] = 1; p[n++] = 0;                                 /* one AKM */
    memcpy(p + n, "\x00\x0f\xac", 3); p[n + 3] = (uint8_t)jakm; n += 4;
    uint16_t caps = jmfp ? 0x0080 : 0;                      /* MFP capable */
    p[n++] = (uint8_t)caps; p[n++] = (uint8_t)(caps >> 8);
    if (jmfp) {                                             /* PMKID count 0, then BIP-CMAC-128 */
        p[n++] = 0; p[n++] = 0;
        memcpy(p + n, "\x00\x0f\xac\x06", 4); n += 4;
    }
    p[lp] = (uint8_t)(n - 2);
    return n;
}

static void send_assoc(void)
{
    uint8_t f[384];
    size_t n = mgmt_hdr(f, 0x00);
    uint16_t cap = 0x0001 | 0x0020;                         /* ESS, short preamble */
    if (is2g(jb.freq) && (jb.cap & 0x0400)) cap |= 0x0400; /* short slot time */
    f[n++] = (uint8_t)cap; f[n++] = (uint8_t)(cap >> 8);
    f[n++] = 1; f[n++] = 0;                                 /* listen interval */
    size_t sl = strlen(jb.ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, jb.ssid, sl); n += sl;
    uint8_t r1 = jb.nrates < 8 ? jb.nrates : 8;
    f[n++] = 1; f[n++] = r1; memcpy(f + n, jb.rates, r1); n += r1;
    if (jb.nrates > 8) { f[n++] = 50; f[n++] = (uint8_t)(jb.nrates - 8); memcpy(f + n, jb.rates + 8, jb.nrates - 8u); n += jb.nrates - 8u; }
    if (jb.secure == 2) n += build_rsn_ie(f + n);
    int use_vht = !is2g(jb.freq) && jb.has_vht;
    if (jb.has_ht) {
        /* WMM Information Element: vendor 221, length 7, OUI 00:50:f2:02, subtype 0 (WME info), ver 1, qos 0 */
        static const uint8_t wmm_ie[9] = { 221, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00 };
        memcpy(f + n, wmm_ie, sizeof wmm_ie); n += sizeof wmm_ie;
        /* HT Capabilities Element: tag 45, length 26 */
        f[n++] = 45; f[n++] = 26;
        uint16_t htcap = 0x002c;                            /* SMPS disabled (0xc) | SGI20 (0x20) */
        if (use_vht || (jb.ht_cap & 0x0002)) htcap |= 0x0062; /* 40MHz support (0x02) | SGI40 (0x40) */
        if (jb.ht_cap & 0x1000) htcap |= 0x1000;            /* DSSS/CCK in 40MHz */
        if (jb.ht_cap & 0x0001) htcap |= 0x0001;            /* LDPC if AP supports it */
        if (jb.ht_cap & 0x0300) htcap |= 0x0100;            /* 1-stream Rx STBC */
        f[n++] = (uint8_t)htcap; f[n++] = (uint8_t)(htcap >> 8);
        f[n++] = 0x1b;                                      /* AMPDU: max 64K, 8us density */
        f[n++] = 0xff;                                      /* MCS 0..7 supported */
        f[n++] = (hw_rf_chains >= 2 && jb.ht_mcs1) ? 0xff : 0x00; /* MCS 8..15 only if hardware has 2+ chains */
        memset(f + n, 0, 21); n += 21;                      /* MCS 16..76 zero, extended caps, BF, ASEL */
    }
    if (use_vht) {
        /* VHT Capabilities Element: tag 191, length 12 */
        f[n++] = 191; f[n++] = 12;
        uint32_t vcap = (hw_vht_cap ? hw_vht_cap : 0x03800000u) | 0x0030u; /* Short GI 80 (0x20) | Rx LDPC (0x10) */
        if (jb.vht_cap & 0x00000100u) vcap |= 0x00000100u;                  /* Rx STBC 1 stream */
        f[n++] = (uint8_t)vcap; f[n++] = (uint8_t)(vcap >> 8);
        f[n++] = (uint8_t)(vcap >> 16); f[n++] = (uint8_t)(vcap >> 24);
        int nss = (hw_rf_chains >= 2 && (jb.vht_rx_mcs & (3 << 2)) != (3 << 2)) ? 2 : 1;
        uint16_t mcs_map = nss == 2 ? 0xfffa : 0xfffe;
        if (jb.vht_rx_mcs) {
            uint32_t m1 = jb.vht_rx_mcs & 3;
            uint32_t m2 = (jb.vht_rx_mcs >> 2) & 3;
            if (m1 < 3) mcs_map = (mcs_map & ~3u) | (uint16_t)m1;
            if (nss >= 2 && m2 < 3) mcs_map = (mcs_map & ~(3u << 2)) | (uint16_t)(m2 << 2);
        }
        f[n++] = (uint8_t)mcs_map; f[n++] = (uint8_t)(mcs_map >> 8);
        f[n++] = 0; f[n++] = 0;                             /* rx_highest: 0 (governed by MCS map) */
        f[n++] = (uint8_t)mcs_map; f[n++] = (uint8_t)(mcs_map >> 8);
        f[n++] = 0; f[n++] = 0;                             /* tx_highest: 0 */
    }
    mgmt_tx(f, n);
    jsent = timer_ms();
}

/* The rates both have, as the firmware wants them: 500 kb/s units, CCK marked, plus HT MCS. */
static void send_peer_assoc(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_PEER_ASSOC, 76);
    put_mac(c, jb.bssid);
    put32(c, 2, 0);                                         /* vdev */
    put32(c, 3, 1);                                         /* a new association */
    put32(c, 4, jaid);
    uint32_t flags = 1u /* AUTH */ | (jb.secure == 2 ? PEER_NEED_PTK_4_WAY : 0u)
                   | (jmfp ? PEER_PMF | PEER_NEED_GTK_2_WAY : 0u);
    int use_vht = !is2g(jb.freq) && jb.has_vht;
    if (jb.has_ht) {
        flags |= PEER_HT | PEER_QOS;
        if (jb.ht_cap & 0x0001) flags |= PEER_LDPC;
        if (jb.ht_cap & 0x0380) flags |= PEER_STBC;
    }
    if (use_vht) {
        flags |= 0x00002000u /* WMI_TLV_PEER_40MHZ */
              |  0x04000000u /* WMI_TLV_PEER_80MHZ */
              |  0x02000000u /* WMI_TLV_PEER_VHT */;
    }
    put32(c, 5, flags);
    put32(c, 6, jcap);
    put32(c, 7, 1);                                         /* listen interval */
    int nss = (hw_rf_chains >= 2 && (use_vht ? ((jb.vht_rx_mcs & (3 << 2)) != (3 << 2)) : jb.ht_mcs1)) ? 2 : 1;
    int num_ht = (jb.has_ht ? (nss == 2 ? 16 : 8) : 0);
    if (jb.has_ht) {
        uint32_t peer_ht = (uint32_t)jb.ht_cap;
        if (use_vht) peer_ht |= 0x0062u;                    /* 40MHz, SGI20, SGI40 */
        put32(c, 8, peer_ht);                               /* ht_caps */
        uint32_t max_mpdu = (1u << (13 + (jb.ampdu_param ? (jb.ampdu_param & 3) : 3))) - 1;
        put32(c, 9, max_mpdu);                              /* max_mpdu (e.g. 65535) */
        uint8_t d = (jb.ampdu_param >> 2) & 7;
        put32(c, 10, d == 0 ? 0 : d <= 3 ? 1 : 1u << (d - 3)); /* mpdu_density */
        uint32_t rc = 0x08;                                 /* WMI_RC_HT_FLAG */
        if (use_vht) {
            rc |= 0x02;                                     /* WMI_RC_CW40_FLAG */
            rc |= 0x04;                                     /* WMI_RC_SGI_FLAG */
        }
        if (nss >= 2) rc |= 0x01;                           /* WMI_RC_DS_FLAG (Dual Stream) */
        if (jb.ht_cap & 0x0020) rc |= 0x04;                 /* WMI_RC_SGI_FLAG */
        if (jb.ht_cap & 0x0080) rc |= 0x20;                 /* WMI_RC_TX_STBC_FLAG */
        if (jb.ht_cap & 0x0300) rc |= ((jb.ht_cap & 0x0300) >> 8) << 6;
        put32(c, 11, rc);                                   /* rate_caps */
    }
    put32(c, 12, (uint32_t)nss);                            /* spatial streams (1 or 2) */
    kprintf("ath: bss: ht %04x vht_cap %08x rx_mcs %04x width %d cf1 %d nss %d\n",
            jb.ht_cap, jb.vht_cap, jb.vht_rx_mcs, jb.vht_width, jb.vht_cf1, nss);
    if (use_vht) {
        put32(c, 13, jb.vht_cap ? jb.vht_cap : hw_vht_cap); /* vht_caps */
        put32(c, 14, 10u /* MODE_11AC_VHT80 */);            /* phy_mode */
    } else {
        put32(c, 14, is2g(jb.freq) ? (jb.has_ht ? 5u /* MODE_11NG_HT20 */ : 1u /* MODE_11G */)
                                   : (jb.has_ht ? 4u /* MODE_11NA_HT20 */ : 0u /* MODE_11A */));
    }
    uint8_t rates[16];
    int nr = 0;
    for (int i = 0; i < jb.nrates; i++) {
        uint8_t r = jb.rates[i] & 0x7f;
        if (r == 0x7f || !r) continue;                      /* membership selectors */
        rates[nr++] = (uint8_t)(r | (r == 2 || r == 4 || r == 11 || r == 22 ? 0x80 : 0));
    }
    put32(c, 17, (uint32_t)nr);
    put32(c, 18, (uint32_t)num_ht);                         /* MCS 0..7 or MCS 0..15 */
    uint8_t *lr = tlv_put(&t, TAG_ARRAY_BYTE, ((size_t)nr + 3) & ~(size_t)3);
    memcpy(lr, rates, (size_t)nr);
    if (num_ht) {
        static const uint8_t ht_rates[16] = {
            0, 1, 2, 3, 4, 5, 6, 7,
            8, 9, 10, 11, 12, 13, 14, 15
        };
        uint8_t *hr = tlv_put(&t, TAG_ARRAY_BYTE, (size_t)num_ht);
        memcpy(hr, ht_rates, (size_t)num_ht);
    } else {
        tlv_put(&t, TAG_ARRAY_BYTE, 0);
    }
    uint32_t *vht = tlv_put(&t, TAG_VHT_RATE_SET, 16);
    if (use_vht) {
        uint32_t mcs_map = nss >= 2 ? 0xfffa : 0xfffe;
        if (jb.vht_rx_mcs) {
            uint32_t m1 = jb.vht_rx_mcs & 3;
            uint32_t m2 = (jb.vht_rx_mcs >> 2) & 3;
            if (m1 < 3) mcs_map = (mcs_map & ~3u) | m1;
            if (nss >= 2 && m2 < 3) mcs_map = (mcs_map & ~(3u << 2)) | (m2 << 2);
        }
        uint32_t max_rate = nss >= 2 ? 867 : 433;
        put32(vht, 0, max_rate);  /* rx_max_rate */
        put32(vht, 1, mcs_map);   /* rx_mcs_set */
        put32(vht, 2, max_rate);  /* tx_max_rate */
        put32(vht, 3, mcs_map);   /* tx_mcs_set */
    }
    wmi_send(WMI_PEER_ASSOC, t.b, t.n);
}

static void send_vdev_up(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_VDEV_UP, 16);
    put32(c, 0, 0);
    put32(c, 1, jaid);
    put_mac(c + 2, jb.bssid);
    wmi_send(WMI_VDEV_UP, t.b, t.n);
}

static void send_ps_off(void)
{
    static struct tb t;
    t.n = 0;
    uint32_t *c = tlv_put(&t, TAG_STA_PS_MODE, 8);
    put32(c, 0, 0);
    put32(c, 1, 0);                                         /* power save off */
    wmi_send(WMI_STA_POWERSAVE_MODE, t.b, t.n);
}

/* ---- the 4-way handshake ------------------------------------------------------------------ */
/* An EAPOL-Key frame, offsets from the 802.1X header (version, type, length,
 * then the key descriptor). The MIC covers the whole frame with its own MIC
 * field zeroed (WPA2 with PSK: key descriptor version 2, HMAC-SHA1-128). */
#define EAP_KEYINFO 5
#define EAP_KEYLEN  7
#define EAP_REPLAY  9
#define EAP_NONCE   17
#define EAP_RSC     65
#define EAP_MIC     81
#define EAP_KDL     97
#define EAP_KDATA   99

/* The pairwise key: KDF-384(PMK, "Pairwise key expansion",
 * min(AA,SA) || max(AA,SA) || min(ANonce,SNonce) || max(ANonce,SNonce)),
 * the KDF being the SHA-1 PRF for AKM 2 and the SHA-256 KDF for 6 and 8. */
static void derive_ptk(void)
{
    uint8_t data[76];
    int mc = memcmp(jb.bssid, mac, 6);
    memcpy(data + 0, mc < 0 ? jb.bssid : mac, 6);
    memcpy(data + 6, mc < 0 ? mac : jb.bssid, 6);
    int nc = memcmp(anonce, snonce, 32);
    memcpy(data + 12, nc < 0 ? anonce : snonce, 32);
    memcpy(data + 44, nc < 0 ? snonce : anonce, 32);
    if (jakm == 2) prf_sha1(pmk, 32, "Pairwise key expansion", data, sizeof data, ptk, sizeof ptk);
    else kdf_sha256(pmk, 32, "Pairwise key expansion", data, sizeof data, ptk, 384);
}

/* The EAPOL-Key descriptor version and MIC, by AKM: HMAC-SHA1-128 for AKM
 * 2 (version 2), AES-128-CMAC for the SHA-256 AKMs (6: version 3, 8: the
 * version field 0, "AKM defined"). The MIC key is the KCK (PTK's first 16). */
static int kd_version(void) { return jakm == 2 ? 2 : jakm == 6 ? 3 : 0; }
static void eapol_mic(const uint8_t *buf, size_t len, uint8_t out[16])
{
    if (jakm == 2) { uint8_t h[20]; hmac_sha1(ptk, 16, buf, len, h); memcpy(out, h, 16); }
    else aes_cmac(ptk, buf, len, out);
}

/* Build an EAPOL-Key frame inside a native-Wi-Fi data frame to the access
 * point (802.11 header to-DS, LLC/SNAP for the EAPOL ethertype), MIC last. */
static size_t build_eapol(uint8_t *f, uint16_t flags, int keylen, const uint8_t *nonce, const uint8_t *kdata, size_t kdlen)
{
    f[0] = 0x08; f[1] = 0x01; f[2] = f[3] = 0;              /* data, to-DS */
    memcpy(f + 4, jb.bssid, 6); memcpy(f + 10, mac, 6); memcpy(f + 16, jb.bssid, 6);
    f[22] = f[23] = 0;
    static const uint8_t llc[8] = { 0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e };
    memcpy(f + 24, llc, 8);
    uint8_t *e = f + 32;
    uint16_t keyinfo = (uint16_t)(flags | kd_version());    /* the AKM's key descriptor version */
    size_t total = EAP_KDATA + kdlen;
    memset(e, 0, total);
    e[0] = 1; e[1] = 3;                                     /* 802.1X 2001, EAPOL-Key */
    e[2] = (uint8_t)((total - 4) >> 8); e[3] = (uint8_t)(total - 4);
    e[4] = 2;                                               /* RSN key descriptor */
    e[EAP_KEYINFO] = (uint8_t)(keyinfo >> 8); e[EAP_KEYINFO + 1] = (uint8_t)keyinfo;
    e[EAP_KEYLEN] = (uint8_t)(keylen >> 8); e[EAP_KEYLEN + 1] = (uint8_t)keylen;
    memcpy(e + EAP_REPLAY, replay, 8);
    if (nonce) memcpy(e + EAP_NONCE, nonce, 32);
    e[EAP_KDL] = (uint8_t)(kdlen >> 8); e[EAP_KDL + 1] = (uint8_t)kdlen;
    if (kdlen) memcpy(e + EAP_KDATA, kdata, kdlen);
    if (keyinfo & 0x100) eapol_mic(e, total, e + EAP_MIC);  /* MIC bit set: over the frame with it zeroed */
    return 32 + total;
}

/* The key data of a handshake message (unwrapped): key descriptor elements.
 * The GTK rides in a GTK KDE (00-0f-ac type 1); with PMF the IGTK (the group
 * management key) rides in an IGTK KDE (type 9: key id (2), IPN (6), key). */
static void parse_keys(const uint8_t *p, size_t n)
{
    for (size_t o = 0; o + 2 <= n; ) {
        uint8_t id = p[o], l = p[o + 1];
        if (o + 2 + (size_t)l > n) break;
        const uint8_t *v = p + o + 2;
        if (id == 0xdd && l >= 6 && v[0] == 0x00 && v[1] == 0x0f && v[2] == 0xac) {
            if (v[3] == 0x01 && l >= 8) {                   /* GTK KDE */
                gtk_id = v[4] & 3;
                gtk_len = l - 6;
                if (gtk_len > (int)sizeof gtk) gtk_len = sizeof gtk;
                memcpy(gtk, v + 6, (size_t)gtk_len);
            } else if (v[3] == 0x09 && l >= 4 + 2 + 6 + 16) {   /* IGTK KDE */
                igtk_id = (int)(v[4] | v[5] << 8);
                memcpy(igtk, v + 4 + 2 + 6, 16);            /* after key id and the 6-byte IPN */
            }
        }
        o += 2 + (size_t)l;
    }
}

/* One key into the firmware (WMI_VDEV_INSTALL_KEY); the radio does the
 * per-packet cipher (CCMP for data, BIP-CMAC for group management). */
static void install_key(uint32_t idx, uint32_t flags, uint32_t cipher, const uint8_t *key, uint32_t klen, const uint8_t *rsc)
{
    static struct tb t;
    t.n = 0;
    uint8_t *c = tlv_put(&t, TAG_VDEV_INSTALL_KEY, 92);
    memcpy(c + 4, jb.bssid, 6);                             /* peer_macaddr (vdev 0 at +0) */
    put32(c + 12, 0, idx);
    put32(c + 16, 0, flags);
    put32(c + 20, 0, cipher);
    if (rsc) memcpy(c + 24, rsc, 8);                        /* key_rsc_counter */
    put32(c + 80, 0, klen);                                 /* key_len (txmic/rxmic 0) */
    uint8_t *kd = tlv_put(&t, TAG_ARRAY_BYTE, (klen + 3) & ~3u);
    memcpy(kd, key, klen);
    wmi_send(WMI_VDEV_INSTALL_KEY, t.b, t.n);
}

/* The group key (and, with PMF, the group management key) into the radio. */
static void install_group_keys(void)
{
    if (gtk_len) install_key((uint32_t)gtk_id, WMI_KEY_GROUP, WMI_CIPHER_AES_CCM, gtk, (uint32_t)gtk_len, gtk_rsc);
    if (jmfp && igtk_id) install_key((uint32_t)igtk_id, WMI_KEY_GROUP, WMI_CIPHER_AES_CMAC, igtk, 16, NULL);
}

/* A frame carrying a replay counter not newer than the last we accepted is
 * a replay (an equal one is a retransmit, which we still answer). */
static int replay_old(const uint8_t *r) { return memcmp(r, rx_replay, 8) < 0; }

/* EAPOL-Key frames from the access point: the pairwise 4-way (messages 1
 * and 3) while joining, and once connected the group key 2-way handshake
 * (a group message 1) and any retransmitted message 3. */
static void eapol_rx(const uint8_t *e, size_t n)
{
    if (n < EAP_KDATA || e[1] != 3) return;
    if (jstate != J_KEYS && jstate != J_UP) return;
    uint16_t ki = (uint16_t)(e[EAP_KEYINFO] << 8 | e[EAP_KEYINFO + 1]);
    size_t kdl = (size_t)(e[EAP_KDL] << 8 | e[EAP_KDL + 1]);
    int pairwise = (ki & 0x08) != 0, mic = (ki & 0x100) != 0;
    eapol_rx_n++;

    if (pairwise && !mic) {                                 /* message 1 of the 4-way: ANonce, no MIC */
        if (jstate != J_KEYS) return;
        memcpy(anonce, e + EAP_NONCE, 32);
        random_bytes(snonce, sizeof snonce);
        derive_ptk();
        memcpy(replay, e + EAP_REPLAY, 8);
        memcpy(rx_replay, e + EAP_REPLAY, 8);
        uint8_t f[256], ie[40];
        size_t fl = build_eapol(f, 0x108, 16, snonce, ie, build_rsn_ie(ie));   /* pairwise, MIC */
        if (htt_tx_data(f, fl, 0) == 0) eapol_tx_n++;
        hs = 1; jsent = timer_ms();
        return;
    }
    if (!mic) return;                                       /* every later message carries a MIC */
    if (replay_old(e + EAP_REPLAY)) { eapol_replays++; return; }

    /* verify the MIC over the frame with the MIC field zeroed */
    size_t total = 4 + (size_t)(e[2] << 8 | e[3]);
    if (total > n) total = n;
    uint8_t copy[512], mic16[16];
    if (total > sizeof copy) return;
    memcpy(copy, e, total);
    memset(copy + EAP_MIC, 0, 16);
    eapol_mic(copy, total, mic16);
    if (memcmp(mic16, e + EAP_MIC, 16)) {
        if (jstate == J_KEYS) join_fail("the handshake failed (wrong passphrase?)", 0);
        return;
    }
    memcpy(rx_replay, e + EAP_REPLAY, 8);

    /* the encrypted key data, unwrapped with the KEK (NIST AES key wrap) */
    uint8_t plain[512];
    size_t plen = 0;
    if ((ki & 0x1000) && kdl >= 24 && kdl % 8 == 0 && EAP_KDATA + kdl <= n) {
        int nb = (int)(kdl / 8) - 1;
        if ((size_t)nb * 8 <= sizeof plain && aes_unwrap(ptk + 16, nb, e + EAP_KDATA, plain) == 0) plen = (size_t)nb * 8;
    }

    if (pairwise && (ki & 0x40)) {                          /* message 3: completes the 4-way */
        int first = hs != 2;                                /* otherwise a retransmit: re-answer only */
        if (first && plen) { gtk_len = 0; igtk_id = 0; parse_keys(plain, plen); }
        memcpy(gtk_rsc, e + EAP_RSC, 8);
        memcpy(replay, e + EAP_REPLAY, 8);
        uint8_t f[256];
        size_t fl = build_eapol(f, 0x308, 0, NULL, NULL, 0);   /* message 4: pairwise, MIC, secure */
        if (htt_tx_data(f, fl, 0) == 0) eapol_tx_n++;          /* in the clear, then the keys */
        if (first) {
            install_key(0, WMI_KEY_PAIRWISE, WMI_CIPHER_AES_CCM, ptk + 32, 16, NULL);
            install_group_keys();
            send_peer_param(3, 1);                          /* authorize the controlled port */
            hs = 2; jstate = J_UP; reconn_tries = 0;
            wlan0_up();
            kprintf("ath: join: connected to %s (%s)\n", jb.ssid, jakm == 8 ? "WPA3" : "WPA2");
        }
        return;
    }
    if (!pairwise && hs == 2) {                             /* group key handshake: a new GTK, install and ack */
        if (plen) { parse_keys(plain, plen); install_group_keys(); }
        memcpy(gtk_rsc, e + EAP_RSC, 8);
        memcpy(replay, e + EAP_REPLAY, 8);
        uint8_t f[128];
        size_t fl = build_eapol(f, 0x300, 0, NULL, NULL, 0);   /* group, MIC, secure */
        if (htt_tx_data(f, fl, 0) == 0) eapol_tx_n++;
        group_rekeys++;
    }
}

/* A received data frame (native Wi-Fi: 802.11 header, LLC/SNAP, payload).
 * Only EAPOL matters to the driver; the rest is counted for the report. */
/* One de-encapsulated MSDU (its LLC/SNAP payload) with its addresses:
 * EAPOL goes to the handshake, IP and ARP up to wlan0, the rest counted. */
static void deliver_llc(const uint8_t *da, const uint8_t *sa, const uint8_t *p, size_t n)
{
    if (n < 8 || !(p[0] == 0xaa && p[1] == 0xaa && p[2] == 0x03)) { rx_other++; return; }
    uint16_t ether = (uint16_t)(p[6] << 8 | p[7]);
    rx_last_type = ether;
    const uint8_t *pl = p + 8;
    size_t pn = n - 8;
    if (ether == 0x888e) { rx_eapol++; eapol_rx(pl, pn); return; }
    if (ether == 0x0806) rx_arp++;
    else if (ether == 0x0800) rx_ip++;
    else { rx_other++; return; }
    wlan0_in(da, sa, ether, pl, pn);
}

/* A received data frame (native Wi-Fi). QoS data carries a two-byte QoS
 * control; when its A-MSDU bit is set the body is a run of aggregated
 * subframes (DA, SA, length, payload, padded to four bytes). */
static void wlan_frame(const uint8_t *f, size_t n, int decrypted)
{
    (void)decrypted;
    if (n < 24 || (f[0] & 0x0c) != 0x08) { rx_other++; return; }   /* data frames only */
    int qos = (f[0] & 0x80) != 0;
    size_t hdr = qos ? 26 : 24;
    if (n < hdr) { rx_bad++; return; }
    if (qos) rx_qos++;
    const uint8_t *da = f + 4, *sa = (f[1] & 3) == 2 ? f + 16 : f + 10;   /* from-DS: DA = addr1, SA = addr3 */
    if (qos && (f[24] & 0x80)) {                           /* A-MSDU */
        const uint8_t *p = f + hdr;
        size_t rem = n - hdr;
        while (rem >= 14) {
            size_t sl = (size_t)(p[12] << 8 | p[13]);
            if (14 + sl > rem) break;
            rx_amsdu++;
            deliver_llc(p, p + 6, p + 14, sl);             /* each subframe names its own DA and SA */
            size_t adv = (14 + sl + 3) & ~(size_t)3;
            if (adv >= rem) break;
            p += adv; rem -= adv;
        }
        return;
    }
    deliver_llc(da, sa, f + hdr, n - hdr);
}

/* ---- wlan0 ----------------------------------------------------------------------------------- */
/* The connection as an Ethernet interface. The radio's side runs on the
 * IPC thread, the network stack on its own (with net_lock held), so
 * frames cross in two small queues under a lock of their own:
 *   in   IP and ARP from the access point, as Ethernet frames; the
 *        stack's poll takes them
 *   out  what the stack sends, Ethernet; the IPC thread's poll turns it
 *        into a native Wi-Fi frame to the access point (to-DS, LLC/SNAP)
 *        and the radio encrypts it */

#define WQ_SLOTS 64
#define WQ_BUF   1600
struct wq { uint8_t *buf[WQ_SLOTS]; uint16_t len[WQ_SLOTS]; unsigned head, tail; };
static struct wq q_in, q_out;
static spinlock_t q_lock;
static struct netdev wlan0;
static int wlan0_registered;
static uint32_t in_drop, out_drop, out_sent;

static int wq_init(struct wq *q)
{
    if (q->buf[0]) return 0;
    for (int i = 0; i < WQ_SLOTS; i++) if (!(q->buf[i] = kmalloc(WQ_BUF))) return -1;
    return 0;
}

/* a frame in, from any thread; -1 when full */
static int wq_put(struct wq *q, const void *a, size_t an, const void *b, size_t bn)
{
    if (!q->buf[0] || an + bn > WQ_BUF) return -1;
    spin_lock(&q_lock);
    int ok = q->tail - q->head < WQ_SLOTS;
    if (ok) {
        unsigned i = q->tail % WQ_SLOTS;
        memcpy(q->buf[i], a, an);
        if (bn) memcpy(q->buf[i] + an, b, bn);
        q->len[i] = (uint16_t)(an + bn);
        q->tail++;
    }
    spin_unlock(&q_lock);
    return ok ? 0 : -1;
}

/* the oldest frame, copied out; 0 when empty */
static size_t wq_get(struct wq *q, uint8_t *out)
{
    size_t n = 0;
    spin_lock(&q_lock);
    if (q->head != q->tail) {
        unsigned i = q->head % WQ_SLOTS;
        n = q->len[i];
        memcpy(out, q->buf[i], n);
        q->head++;
    }
    spin_unlock(&q_lock);
    return n;
}

/* From wlan_frame: dst, src and type with the payload, as Ethernet. */
static void wlan0_in(const uint8_t *dst, const uint8_t *src, uint16_t type, const uint8_t *p, size_t n)
{
    if (!wlan0_registered || jstate != J_UP) return;
    if (!memcmp(src, mac, 6)) return;                       /* our own broadcasts, back from the access point */
    uint8_t h[14];
    memcpy(h, dst, 6); memcpy(h + 6, src, 6);
    h[12] = (uint8_t)(type >> 8); h[13] = (uint8_t)type;
    if (wq_put(&q_in, h, 14, p, n)) in_drop++;
    else netdev_kick(&wlan0);
}

/* The stack's side: net_lock held. */
static void wlan0_poll(struct netdev *d)
{
    static uint8_t f[WQ_BUF];
    for (int k = 0; k < WQ_SLOTS; k++) {
        size_t n = wq_get(&q_in, f);
        if (!n) break;
        struct pkt *p = pkt_alloc();
        if (!p) { d->rx_dropped++; continue; }
        if (n > pkt_tailroom(p)) { pkt_free(p); d->rx_dropped++; continue; }
        memcpy(pkt_data(p), f, n);
        p->len = (uint32_t)n;
        net_rx(d, p);
    }
}

static int wlan0_xmit(struct netdev *d, struct pkt *p)
{
    int rc = 0;
    if (jstate != J_UP || p->len < 14 || wq_put(&q_out, pkt_data(p), p->len, NULL, 0)) { d->tx_errors++; out_drop++; rc = -1; }
    pkt_free(p);
    return rc;
}

/* The IPC thread's side: Ethernet out as 802.11 to the access point. */
static void wlan0_send(void)
{
    static uint8_t e[WQ_BUF], f[WQ_BUF + 32];
    for (int k = 0; k < 16 && htt_tx_n - htt_tx_compl < HTT_TX_SLOTS; k++) {
        size_t n = wq_get(&q_out, e);
        if (!n) break;
        if (jstate != J_UP) { out_drop++; continue; }
        f[0] = 0x08; f[1] = 0x01; f[2] = f[3] = 0;          /* data, to-DS */
        memcpy(f + 4, jb.bssid, 6);                         /* receiver: the access point */
        memcpy(f + 10, mac, 6);                             /* transmitter: us */
        memcpy(f + 16, e, 6);                               /* the destination */
        f[22] = f[23] = 0;                                  /* sequence: the firmware's */
        static const uint8_t snap[6] = { 0xaa, 0xaa, 3, 0, 0, 0 };
        memcpy(f + 24, snap, 6);
        memcpy(f + 30, e + 12, n - 12);                     /* type, then the payload */
        if (htt_tx_data(f, 30 + n - 12, 1)) out_drop++; else out_sent++;
    }
}

static void wlan0_up(void)
{
    if (!wlan0_registered) {
        if (wq_init(&q_in) || wq_init(&q_out)) { kprintf("ath: no memory for wlan0\n"); return; }
        strcpy(wlan0.name, "wlan0");
        wlan0.xmit = wlan0_xmit;
        wlan0.poll = wlan0_poll;
        wlan0.mtu = 1500;
        netdev_register(&wlan0);
        wlan0_registered = 1;
    }
    memcpy(wlan0.mac, mac, 6);
    wlan0.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING;
    kprintf("ath: wlan0 up (run dhcp)\n");
}

static void wlan0_down(void)
{
    if (wlan0_registered) wlan0.flags &= ~IFF_RUNNING;
}

/* A network that can be joined: open, or RSN with CCMP and a personal AKM
 * we support (PSK, PSK-SHA256, or SAE). Protected management frames and
 * WPA3 are fine now. */
static const char *usable(const struct bss *b)
{
    if (b->secure == 1) return "it is WPA (version 1) only";
    if (b->secure == 2 && !b->ccmp) return "it does not offer CCMP";
    if (b->secure == 2 && !(b->psk || b->akm_psk256 || b->akm_sae)) return "it offers no personal AKM we support";
    if (!b->nrates) return "it named no rates";
    return NULL;
}

/* The strongest access point with that name. */
static int join_pick(void)
{
    int best = -1;
    const char *why_not = NULL;
    for (int i = 0; i < nbss; i++) {
        if (strcmp(bsses[i].ssid, jssid)) continue;
        const char *u = usable(&bsses[i]);
        if (u) { why_not = u; continue; }
        if (best < 0 || bss_score(&bsses[i]) > bss_score(&bsses[best])) best = i;
    }
    if (best < 0) {
        if (why_not) {
            ksnprintf(jwhy, sizeof jwhy, "%s", why_not);
            kprintf("ath: join: %s: %s\n", jssid, jwhy);
            jstate = J_FAILED;
        }
        return -1;
    }
    jb = bsses[best];
    hs = 0; gtk_len = 0; igtk_id = 0; memset(rx_replay, 0, 8);
    if (jb.secure == 2) {
        if (!jpass[0]) { join_fail("the network needs a passphrase", 0); return -1; }
        jakm = jb.akm_sae ? 8 : jb.psk ? 2 : 6;             /* prefer WPA3 (SAE), then WPA2-PSK, then PSK-SHA256 */
        jmfp = jakm == 8 || jb.mfp_req;                     /* SAE requires PMF; honour a required bit otherwise */
        if (jakm != 8) pbkdf2_sha1(jpass, (const uint8_t *)jb.ssid, strlen(jb.ssid), 4096, pmk, 32);
    } else { jakm = 0; jmfp = 0; }
    const char *phy = "";
    if (!is2g(jb.freq) && jb.has_vht) {
        phy = ((hw_rf_chains ? hw_rf_chains : 2) >= 2 && (jb.vht_rx_mcs & (3 << 2)) != (3 << 2))
              ? ", 802.11ac VHT80 2x2 MIMO" : ", 802.11ac VHT80";
    } else if (jb.has_ht) {
        phy = (jb.ht_mcs1 ? ", 802.11n 2x2 MIMO" : ", 802.11n");
    }
    kprintf("ath: join: %s at %02x:%02x:%02x:%02x:%02x:%02x, %d MHz, %d dBm, %s%s\n", jb.ssid, jb.bssid[0], jb.bssid[1], jb.bssid[2],
            jb.bssid[3], jb.bssid[4], jb.bssid[5], jb.freq, jb.rssi,
            jakm == 8 ? "WPA3-SAE" : jakm == 6 ? "WPA2-SHA256" : jakm == 2 ? "WPA2-PSK" : "open",
            phy);
    jstate = J_START;
    jsent = timer_ms();
    send_vdev_start();
    return 0;
}

static void join_saw(struct bss *b)
{
    (void)b;
}

/* ---- SAE (WPA3-Personal): the dragonfly over Authentication frames -------------------------
 * Group 19 (NIST P-256). Our commit is a scalar and an element built from a
 * random and a mask; the peer's commit yields a shared secret, and from it
 * the KCK and the PMK; a confirm hash then proves each side holds the
 * passphrase. The PMK feeds the ordinary 4-way handshake (AKM 8). */

static void sae_auth_hdr(uint8_t *f, size_t *n, uint16_t seq)
{
    *n = mgmt_hdr(f, 0xb0);
    f[(*n)++] = 3; f[(*n)++] = 0;                           /* algorithm: SAE */
    f[(*n)++] = (uint8_t)seq; f[(*n)++] = (uint8_t)(seq >> 8);
    f[(*n)++] = 0; f[(*n)++] = 0;                           /* status 0 */
}

static void send_sae_commit(void)
{
    uint8_t f[420];
    size_t n;
    sae_auth_hdr(f, &n, 1);
    f[n++] = 19; f[n++] = 0;                                /* finite cyclic group 19 */
    if (sae_token_len) { memcpy(f + n, sae_token, (size_t)sae_token_len); n += (size_t)sae_token_len; }
    memcpy(f + n, sae_scalar, 32); n += 32;
    memcpy(f + n, sae_element, 64); n += 64;
    mgmt_tx(f, n);
    jsent = timer_ms();
}

static void send_sae_confirm(void)
{
    uint8_t cf[2 + 32 + 64 + 32 + 64], m16[32], f[128];
    size_t c = 0;
    cf[c++] = 1; cf[c++] = 0;                               /* send-confirm counter */
    memcpy(cf + c, sae_scalar, 32); c += 32;
    memcpy(cf + c, sae_element, 64); c += 64;
    memcpy(cf + c, sae_peer_scalar, 32); c += 32;
    memcpy(cf + c, sae_peer_element, 64); c += 64;
    hmac_sha256(sae_kck, 32, cf, c, m16);
    size_t n;
    sae_auth_hdr(f, &n, 2);
    f[n++] = 1; f[n++] = 0;                                 /* send-confirm */
    memcpy(f + n, m16, 32); n += 32;
    mgmt_tx(f, n);
    jsent = timer_ms();
}

static void sae_start(void)
{
    if (p256_sae_pwe(jpass, mac, jb.bssid, sae_pwe)) { join_fail("SAE: no password element", 0); return; }
    if (p256_scalar_rand(sae_rand) || p256_scalar_rand(sae_mask)) { join_fail("SAE: no randomness", 0); return; }
    p256_scalar_add(sae_rand, sae_mask, sae_scalar);        /* commit-scalar = rand + mask */
    uint8_t t[64];
    p256_mul(sae_mask, sae_pwe, t);                         /* commit-element = -(mask * PWE) */
    p256_negate(t, sae_element);
    sae_token_len = 0; sae_confirmed = 0;
    jstate = J_SAE_COMMIT; jtries = 0;
    send_sae_commit();
}

/* The peer's commit (body after the 802.11 header): the shared secret
 * K = rand * (peer-scalar*PWE + peer-element), then keyseed and KCK/PMK. */
static void sae_process_commit(const uint8_t *b, size_t n)
{
    if (n < 6 + 2 + 32 + 64) return;
    if ((b[6] | b[7] << 8) != 19) { join_fail("SAE: the access point wants another group", 0); return; }
    memcpy(sae_peer_scalar, b + 8, 32);
    memcpy(sae_peer_element, b + 40, 64);
    if (!p256_point_valid(sae_peer_element)) { join_fail("SAE: a bad commit element", 0); return; }
    uint8_t t1[64], t2[64], K[64], keyseed[32], zeros[32] = { 0 }, ctx[32], kckpmk[64];
    p256_mul(sae_peer_scalar, sae_pwe, t1);
    p256_add(t1, sae_peer_element, t2);
    p256_mul(sae_rand, t2, K);
    if (!p256_point_valid(K)) { join_fail("SAE: the shared secret is degenerate", 0); return; }
    hmac_sha256(zeros, 32, K, 32, keyseed);                 /* keyseed = H(<0>32, k), k = K.x */
    p256_scalar_add(sae_scalar, sae_peer_scalar, ctx);
    kdf_sha256(keyseed, 32, "SAE KCK and PMK", ctx, 32, kckpmk, 512);
    memcpy(sae_kck, kckpmk, 32);
    memcpy(pmk, kckpmk + 32, 32);
    jstate = J_SAE_CONFIRM; jtries = 0;
    send_sae_confirm();
}

static void sae_process_confirm(const uint8_t *b, size_t n)
{
    if (n < 6 + 2 + 32) return;
    uint8_t cf[2 + 32 + 64 + 32 + 64], m16[32];
    size_t c = 0;
    cf[c++] = b[6]; cf[c++] = b[7];                         /* the peer's send-confirm */
    memcpy(cf + c, sae_peer_scalar, 32); c += 32;
    memcpy(cf + c, sae_peer_element, 64); c += 64;
    memcpy(cf + c, sae_scalar, 32); c += 32;
    memcpy(cf + c, sae_element, 64); c += 64;
    hmac_sha256(sae_kck, 32, cf, c, m16);
    if (memcmp(m16, b + 8, 32)) { join_fail("SAE: the confirm did not verify (wrong passphrase?)", 0); return; }
    sae_confirmed = 1;
    kprintf("ath: join: SAE confirmed\n");
    jstate = J_ASSOC; jtries = 0;
    send_assoc();
}

/* The reconnect after an established link is lost: tear the firmware's vdev
 * and peer down so a fresh scan and join can run. */
static void join_lost(const char *fmt, unsigned a)
{
    wlan0_down();
    ksnprintf(jwhy, sizeof jwhy, fmt, a);
    kprintf("ath: link lost: %s\n", jwhy);
    send_vdev_down();
    send_peer_delete();
    send_vdev_stop();
    hs = 0;
    if (jauto && reconn_tries < 5) {
        reconn_tries++;
        reconn_at = timer_ms() + 1500;
        jstate = J_IDLE;
        kprintf("ath: reconnecting (try %d)\n", reconn_tries);
    } else
        jstate = J_FAILED;
}

/* Management frames to us while joining: authentication, association. */
static void join_mgmt(const uint8_t *f, size_t n)
{
    if (n < 24 || memcmp(f + 4, mac, 6) || memcmp(f + 16, jb.bssid, 6)) return;
    const uint8_t *b = f + 24;
    switch (f[0]) {
    case 0xb0:                                              /* authentication */
        if (n < 30) return;
        if ((b[0] | b[1] << 8) == 3) {                      /* SAE */
            uint16_t seq = (uint16_t)(b[2] | b[3] << 8), status = (uint16_t)(b[4] | b[5] << 8);
            if (seq == 1) {
                if (status == 76 && jstate == J_SAE_COMMIT) {   /* anti-clogging token required: echo it */
                    size_t tl = n - 24 - 8;
                    if (tl > sizeof sae_token) tl = sizeof sae_token;
                    memcpy(sae_token, b + 8, tl); sae_token_len = (int)tl;
                    send_sae_commit();
                } else if (status) join_fail("SAE: the access point rejected the commit (status %u)", status);
                else if (jstate == J_SAE_COMMIT) sae_process_commit(b, n - 24);
            } else if (seq == 2) {
                if (status) join_fail("SAE: the access point rejected the confirm (status %u)", status);
                else if (jstate == J_SAE_CONFIRM) sae_process_confirm(b, n - 24);
            }
            return;
        }
        if (jstate != J_AUTH || (b[2] | b[3] << 8) != 2) return;   /* open system, the response */
        if (b[4] | b[5] << 8) { join_fail("the access point refused authentication (status %u)", (unsigned)(b[4] | b[5] << 8)); return; }
        kprintf("ath: join: authenticated\n");
        jstate = J_ASSOC; jtries = 0;
        send_assoc();
        break;
    case 0x10:                                              /* association response */
        if (jstate != J_ASSOC || n < 30) return;
        if (b[2] | b[3] << 8) { join_fail("the access point refused the association (status %u)", (unsigned)(b[2] | b[3] << 8)); return; }
        jcap = (uint16_t)(b[0] | b[1] << 8);
        int nss_vht = ((hw_rf_chains ? hw_rf_chains : 2) >= 2 && (jb.vht_rx_mcs & (3 << 2)) != (3 << 2)) ? 2 : 1;
        const char *assoc_phy = (!is2g(jb.freq) && jb.has_vht)
            ? (nss_vht == 2 ? " (802.11ac VHT80 2x2 MIMO)" : " (802.11ac VHT80)")
            : ((jb.has_ht && jb.ht_mcs1) ? " (802.11n 2x2 MIMO)" : jb.has_ht ? " (802.11n HT)" : "");
        kprintf("ath: join: associated, id %u%s\n", jaid, assoc_phy);
        send_peer_assoc();
        send_vdev_up();
        if (jb.secure == 2) { jstate = J_KEYS; jsent = timer_ms(); }
        else { send_peer_param(3, 1); jstate = J_UP; reconn_tries = 0; kprintf("ath: join: connected to %s (open)\n", jb.ssid); wlan0_up(); }
        break;
    case 0xc0: case 0xa0:                                   /* deauthentication, disassociation */
        if (jstate == J_UP) join_lost("the access point sent us away (reason %u)", n >= 26 ? (unsigned)(b[0] | b[1] << 8) : 0);
        else if (jstate >= J_START) join_fail("the access point sent us away (reason %u)", n >= 26 ? (unsigned)(b[0] | b[1] << 8) : 0);
        break;
    }
}

static void join_vdev_started(uint32_t status)
{
    if (jstate != J_START) return;
    if (status) { join_fail("the radio did not tune to the channel (status %u)", status); return; }
    send_peer_create();
    send_ps_off();
    if (jakm == 8) sae_start();                             /* WPA3: SAE before the association */
    else { jstate = J_AUTH; jtries = 0; send_auth(); }      /* open or WPA2: open-system authentication */
}

/* Retries and time limits, from the poll. */
static void join_poll(void)
{
    uint64_t now = timer_ms();
    if (jstate == J_IDLE) {                                 /* a pending reconnect */
        if (reconn_at && now >= reconn_at) { reconn_at = 0; jstate = J_FIND; jsent = now; if (!scanning) send_start_scan(); }
        return;
    }
    switch (jstate) {
    case J_FIND:
        if (!scanning) { if (join_pick() && jstate == J_FIND) join_fail("no network named that was found", 0); }
        break;
    case J_START:
        if (now - jsent > 3000) join_fail("no answer to the channel change", 0);
        break;
    case J_SAE_COMMIT:
    case J_SAE_CONFIRM:
        if (now - jsent < 500) break;
        if (++jtries >= 5) { join_fail("no answer to the SAE exchange", 0); break; }
        if (jstate == J_SAE_COMMIT) send_sae_commit(); else send_sae_confirm();
        break;
    case J_AUTH:
    case J_ASSOC:
        if (now - jsent < 500) break;
        if (++jtries >= 5) { join_fail(jstate == J_AUTH ? "no answer to authentication" : "no answer to the association request", 0); break; }
        if (jstate == J_AUTH) send_auth(); else send_assoc();
        break;
    case J_KEYS:
        if (now - jsent > 10000) join_fail("no handshake from the access point", 0);
        break;
    }
}

/* "connect": a fresh scan first unless one just ran, then the strongest of that name. */
int qcom_ath_connect(const char *ssid, const char *pass)
{
    if (stage != A_UP) return -1;
    if (jstate != J_IDLE && jstate != J_FAILED) return -2;
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (!sl || sl > 32 || pl > 63 || (pl && pl < 8)) return -3;
    memcpy(jssid, ssid, sl + 1);
    memcpy(jpass, pass, pl + 1);
    jwhy[0] = 0;
    jauto = 1; reconn_tries = 0; reconn_at = 0;
    jstate = J_FIND;
    if (!vdev_up) { send_chan_list(); send_vdev_create(); vdev_up = 1; }
    if (!scanning) send_start_scan();
    return 0;
}

size_t qcom_ath_join_report(char *buf, size_t len)
{
    if (jstate == J_IDLE) return 0;
    if (jstate == J_FAILED) return (size_t)ksnprintf(buf, len, "join %s: failed: %s\n", jssid, jwhy);
    if (jstate == J_UP) {
        int nss_vht = ((hw_rf_chains ? hw_rf_chains : 2) >= 2 && (jb.vht_rx_mcs & (3 << 2)) != (3 << 2)) ? 2 : 1;
        const char *phy = (!is2g(jb.freq) && jb.has_vht)
            ? (nss_vht == 2 ? " (802.11ac VHT80 2x2 MIMO, 867 Mbps)" : " (802.11ac VHT80, 433 Mbps)")
            : ((jb.has_ht && jb.ht_mcs1) ? " (802.11n 2x2 MIMO, 144 Mbps)" : jb.has_ht ? " (802.11n, 72 Mbps)" : "");
        return (size_t)ksnprintf(buf, len, "join %s: %s%s\n", jssid, join_names[jstate], phy);
    }
    return (size_t)ksnprintf(buf, len, "join %s: %s\n", jssid, join_names[jstate]);
}

int qcom_ath_join_state(void) { return jstate == J_UP ? 2 : jstate == J_FAILED ? -1 : jstate == J_IDLE ? 0 : 1; }

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
    const uint8_t *svc = NULL, *regcaps = NULL, *rdy = NULL, *scan = NULL, *rxhdr = NULL, *frame = NULL, *memreqs = NULL, *chinfo = NULL, *vstart = NULL;
    uint32_t frame_len = 0, memreqs_len = 0;
    while (t + 4 <= end) {
        uint32_t h; memcpy(&h, t, 4);
        uint32_t len = h & 0xffff, tag = h >> 16;
        if (t + 4 + len > end) break;
        const uint8_t *v = t + 4;
        if (tag == TAG_SERVICE_READY && !svc) svc = v;
        else if (tag == TAG_HAL_REG_CAPS && !regcaps && len >= 4) regcaps = v;
        else if (tag == TAG_READY && !rdy) rdy = v;
        else if (tag == TAG_SCAN_EVENT && !scan) scan = v;
        else if (tag == TAG_CHAN_INFO && !chinfo) chinfo = v;
        else if (tag == TAG_MGMT_RX_HDR && !rxhdr) rxhdr = v;
        else if (tag == TAG_VDEV_START_RESP && !vstart && len >= 16) vstart = v;
        else if (tag == TAG_ARRAY_BYTE && !frame) { frame = v; frame_len = len; }
        else if (tag == TAG_ARRAY_STRUCT && !memreqs) { memreqs = v; memreqs_len = len; }
        t += 4 + len;
    }
    switch (id) {
    case EV_SERVICE_READY: {
        if (stage != A_SVC_READY || !svc) break;
        uint32_t nreq; memcpy(&nreq, svc + 4 * 18, 4);       /* after fw_build_vers, abi (6), 11 more */
        memcpy(&hw_rf_chains, svc + 4 * 9, 4);
        memcpy(&hw_vht_cap, svc + 4 * 11, 4);
        memcpy(&hw_vht_mcs, svc + 4 * 12, 4);
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
        if (regcaps) memcpy(&eeprom_rd, regcaps, 4);
        kprintf("ath: service ready (%u memory requests, %d given, regdomain %x, chains %u, vht %08x)\n",
                nreq, nchunks, eeprom_rd, hw_rf_chains, hw_vht_cap);
        send_init();
        stage = A_WMI_READY;
        break;
    }
    case EV_READY:
        if (!rdy) break;
        memcpy(mac, rdy + 24, 6);                           /* after the ABI version (6 words) */
        if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
            /* this firmware leaves it to the host (Linux takes the DT's,
             * or a random one): a locally administered one, the same at
             * every boot, from the SoC's serial data in SMEM */
            size_t sz = 0;
            const volatile uint8_t *si = smem_get(SMEM_GLOBAL_HOST, 137, &sz);
            uint32_t hsh = 2166136261u;
            for (size_t i = 0; si && i < sz && i < 256; i++) hsh = (hsh ^ si[i]) * 16777619u;
            mac[0] = 0x02; mac[1] = 0x51; mac[2] = 0x1c;            /* "sic" */
            mac[3] = (uint8_t)(hsh >> 16); mac[4] = (uint8_t)(hsh >> 8); mac[5] = (uint8_t)hsh;
            mac_made = 1;
        }
        kprintf("ath: WMI ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        stage = A_UP;
        htt_version_req();
        send_pdev_setup();
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
            size_t fl = buflen < frame_len ? buflen : frame_len;
            if (!(status & 1)) { saw_frame(frame, fl, (int)snr - 95, chan_to_freq((int)chan)); join_mgmt(frame, fl); }
        }
        break;
    case EV_VDEV_START_RESP:
        if (vstart) { uint32_t status; memcpy(&status, vstart + 12, 4); join_vdev_started(status); }
        break;
    case EV_MGMT_TX_DONE:
    case EV_MGMT_TX_BUNDLE:
        mgmt_done++;
        break;
    case EV_PEER_KICKOUT:
        if (jstate == J_UP) join_lost("the access point stopped answering", 0);
        else if (jstate >= J_START) join_fail("the access point stopped answering", 0);
        break;
    case EV_CHAN_INFO:
        if (chinfo) {                                       /* err, freq, flags, noise floor, rx clear, cycles */
            int32_t nf; uint32_t freq, clr, cyc;
            memcpy(&freq, chinfo + 4, 4); memcpy(&nf, chinfo + 12, 4); memcpy(&clr, chinfo + 16, 4); memcpy(&cyc, chinfo + 20, 4);
            chinfo_n++;
            if (nf) { if (nf < nf_min) nf_min = nf; if (nf > nf_max) nf_max = nf; }
            if (clr > clear_max) clear_max = clr;
            last_freq = freq; last_cycles = cyc;
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
            for (uint32_t o = 0; o + 4 <= tl; ) {             /* records: id, len, 2 pad, then len bytes */
                uint8_t rid = tr[o], rl = tr[o + 1];
                if (o + 4 + rl > tl) break;
                if (rid == 1)                                   /* credits: eid, credits, 2 pad each */
                    for (uint32_t k = 0; k + 4 <= rl; k += 4) {
                        uint8_t e = tr[o + 4 + k], cr = tr[o + 5 + k];
                        if (e < EP_COUNT) { eps[e].credits += cr; credits_back += cr; }
                    }
                o += 4u + rl;
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
    else if ((int)eid == htt_ep) htt_rx_msg(p, len);
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
    if (stage == A_UP) { join_poll(); if (jstate == J_UP) wlan0_send(); }
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
    if (!pmic_ok && n < len) n += (size_t)ksnprintf(buf + n, len - n, "  rails: the PMIC's LDO registers do not read from here\n");
    if (pmic_ok && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  rails (EN_CTL/STATUS1/V): L8 %02x/%02x/%02x L16 %02x/%02x/%02x L17 %02x/%02x/%02x L23 %02x/%02x/%02x\n",
                               pmic_l[0][0], pmic_l[0][1], pmic_l[0][2], pmic_l[1][0], pmic_l[1][1], pmic_l[1][2],
                               pmic_l[2][0], pmic_l[2][1], pmic_l[2][2], pmic_l[3][0], pmic_l[3][1], pmic_l[3][2]);
    if (stage != A_OFF && n < len) {
        n += (size_t)ksnprintf(buf + n, len - n, "  ce rx:");
        for (int i = 0; i < CE_COUNT && n < len; i++) if (pipes[i].dst_n) n += (size_t)ksnprintf(buf + n, len - n, " %d:%u", i, pipes[i].rx_count);
        if (n < len) n += (size_t)ksnprintf(buf + n, len - n, "; tx: 0:%u 3:%u; htc %u msgs (%u unknown); wmi %u events (%u unknown, last %x), credits %d (%u back), queued %d\n",
                                           pipes[0].tx_count, pipes[3].tx_count, htc_rx, htc_unknown, wmi_events, wmi_unknown, last_unknown,
                                           wmi_ep >= 0 ? eps[wmi_ep].credits : -1, credits_back, wq_len);
    }
    if (stage == A_UP && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  MAC %02x:%02x:%02x:%02x:%02x:%02x%s, %d scans%s, %u frames, %d networks\n",
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac_made ? " (made here)" : "", scans, scanning ? " (scanning)" : "", mgmt_rx, nbss);
    if (stage == A_UP && n < len) n += qcom_ath_join_report(buf + n, len - n);
    if (mgmt_sent && n < len) n += (size_t)ksnprintf(buf + n, len - n, "  management frames: %u sent, %u completions\n", mgmt_sent, mgmt_done);
    if (htt_stage && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  htt: %s %u.%u, %u messages (types %x), %u sent; peer %u, sec %u, frag %u, offload %u, other %u\n",
                               htt_stage == 2 ? "configured" : "version asked", htt_major, htt_minor, htt_rx_msgs, htt_types, pipes[4].tx_count,
                               htt_peer_evts, htt_sec_ind_n, htt_frag_n, htt_offload_n, htt_misc);
    if (chinfo_n && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  channels: %u reports, noise floor %d..%d dBm, busy count up to %u, last %u MHz (%u cycles)\n",
                               chinfo_n, nf_min == 1000 ? 0 : nf_min, nf_max == -1000 ? 0 : nf_max, clear_max, last_freq, last_cycles);
    if (rx_frames && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  data rx: %u frames (%u qos, %u a-msdu sub, %u bad, %u lost, %u offloaded); eapol %u, arp %u, ip %u, other %u (last ethertype %04x)\n",
                               rx_frames, rx_qos, rx_amsdu, rx_bad, rx_lost, rx_offload, rx_eapol, rx_arp, rx_ip, rx_other, rx_last_type);
    if ((eapol_rx_n || htt_tx_n) && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  handshake: %u eapol in, %u out, %u replays dropped; %u group rekeys; htt tx %u (%u done)%s%s\n",
                               eapol_rx_n, eapol_tx_n, eapol_replays, group_rekeys, htt_tx_n, htt_tx_compl,
                               hs == 2 ? ", keys installed" : "", (jakm == 8 && sae_confirmed) ? ", SAE confirmed" : "");
    if (wlan0_registered && n < len)
        n += (size_t)ksnprintf(buf + n, len - n, "  wlan0: %s; out %u sent, %u dropped (%u with every slot busy); in %u dropped\n",
                               wlan0.flags & IFF_RUNNING ? "running" : "down", out_sent, out_drop, htt_tx_busy, in_drop);
    return n;
}
