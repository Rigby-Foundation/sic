/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* UFS (JEDEC UFSHCI): a phone's flash, polled, one request at a time.
 * Each logical unit is a disk (LUN 0 is sda, 1 sdb, ...) of SCSI blocks.
 *
 * A controller found switched off (QEMU's -device ufs) is brought up the
 * standard way: enable, link startup, NOP, fDeviceInit. One the bootloader
 * left running (a Qualcomm phone: its PHY needs a vendor sequence we don't
 * have) is taken over as it is: the transfer list moved to ours, nothing
 * reset. On a phone only LUN 0, where userdata lives, may be written; the
 * others hold the bootloaders and firmware and are read-only here. */
#include "drivers/ufs.h"
#include "drivers/pci.h"
#include "fs/blkdev.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "asm/timer.h"
#include "endian.h"
#include "string.h"
#include "printf.h"

#define REG_CAP     0x00
#define REG_VER     0x08
#define REG_IS      0x20
#define REG_IE      0x24
#define REG_HCS     0x30        /* 0 device present, 1 transfer list ready, 3 UIC command ready */
#define REG_HCE     0x34
#define REG_UTRLBA  0x50
#define REG_UTRLBAU 0x54
#define REG_UTRLDBR 0x58
#define REG_UTRLCLR 0x5c
#define REG_UTRLRSR 0x60
#define REG_UICCMD  0x90
#define REG_UCMDARG1 0x94
#define REG_UCMDARG2 0x98
#define REG_UCMDARG3 0x9c
#define IS_UCCS     (1u << 10)  /* UIC command completed */
#define UIC_LINKSTARTUP 0x16

#define UPIU_NOP_OUT    0x00
#define UPIU_COMMAND    0x01
#define UPIU_QUERY      0x16
#define BOUNCE_PAGES    32      /* up to 128 KiB a request */
#define UCD_RSP_OFF     512
#define UCD_PRDT_OFF    1024
#define MAX_LUNS        8

struct utrd {               /* a transfer request descriptor */
    uint32_t dw0, dw1, dw2, dw3;
    uint32_t ucd_lo, ucd_hi;
    uint16_t rsp_len, rsp_off;
    uint16_t prdt_len, prdt_off;
};

struct ufs {
    volatile uint8_t *regs;
    struct utrd *utrl;      /* 32 slots; slot 0 is all we use */
    uint64_t utrl_phys, ucd_phys, buf_phys;
    uint8_t *ucd, *buf;
    uint8_t tag;
    int keep_running;       /* left on by the bootloader: a phone */
    int probing;            /* asking for LUNs that may not exist: failures are expected */
    struct blkdev luns[MAX_LUNS];
    int lun_ro[MAX_LUNS];
    volatile int busy;
};

static uint32_t rd(struct ufs *u, uint32_t o) { return mmio_read32(u->regs + o); }
static void wr(struct ufs *u, uint32_t o, uint32_t v) { mmio_write32(u->regs + o, v); }
static void lock(struct ufs *u) { while (__atomic_exchange_n(&u->busy, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(struct ufs *u) { __atomic_store_n(&u->busy, 0, __ATOMIC_RELEASE); }

/* The phone's UFS does not snoop the CPU's caches. */
static void cache_clean(const void *p, size_t len)
{
#ifdef __aarch64__
    for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)p; (void)len;
#endif
}
static void cache_inval(const void *p, size_t len)
{
#ifdef __aarch64__
    for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)p; (void)len;
#endif
}

static int wait_bits(struct ufs *u, uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t t0 = timer_ms();
    while ((rd(u, reg) & mask) != want)
        if (timer_ms() - t0 > ms) return -1;
    return 0;
}

/* One request in slot 0: the UPIU in u->ucd, data (if any) through the
 * bounce buffer. dir: 0 none, 1 to the device, 2 from it. */
static int request(struct ufs *u, uint32_t len, int dir)
{
    struct utrd *d = &u->utrl[0];
    memset(d, 0, sizeof *d);
    d->dw0 = htole32(1u << 28 | (uint32_t)dir << 25);           /* UFS storage, data direction */
    d->dw2 = htole32(0x0f);                                     /* OCS: invalid until the controller writes it */
    d->ucd_lo = htole32((uint32_t)u->ucd_phys);
    d->ucd_hi = htole32((uint32_t)(u->ucd_phys >> 32));
    d->rsp_len = htole16(UCD_RSP_OFF / 4);
    d->rsp_off = htole16(UCD_RSP_OFF / 4);
    d->prdt_off = htole16(UCD_PRDT_OFF / 4);
    if (len) {
        uint32_t *prdt = (uint32_t *)(u->ucd + UCD_PRDT_OFF);
        uint32_t n = 0;
        for (uint32_t off = 0; off < len; off += PAGE_SIZE, n++) {
            uint64_t a = u->buf_phys + off;
            uint32_t chunk = len - off < PAGE_SIZE ? len - off : PAGE_SIZE;
            prdt[n * 4 + 0] = htole32((uint32_t)a);
            prdt[n * 4 + 1] = htole32((uint32_t)(a >> 32));
            prdt[n * 4 + 2] = 0;
            prdt[n * 4 + 3] = htole32(chunk - 1);
        }
        d->prdt_len = htole16((uint16_t)n);
        if (dir == 1) cache_clean(u->buf, len);
        else cache_inval(u->buf, len);
    }
    memset(u->ucd + UCD_RSP_OFF, 0, 512);
    cache_clean(u->ucd, UCD_PRDT_OFF + 16 * BOUNCE_PAGES);
    cache_clean(d, sizeof *d);
    wr(u, REG_IS, rd(u, REG_IS));
    wr(u, REG_UTRLDBR, 1);
    int rc = wait_bits(u, REG_UTRLDBR, 1, 0, 2000);
    cache_inval(d, sizeof *d);
    cache_inval(u->ucd + UCD_RSP_OFF, 512);
    if (rc) { wr(u, REG_UTRLCLR, ~1u); kprintf("ufs: request timed out (IS %08x)\n", rd(u, REG_IS)); return -1; }
    if (len && dir == 2) cache_inval(u->buf, len);
    uint32_t ocs = le32toh(d->dw2) & 0xff;
    if (ocs) { if (!u->probing) kprintf("ufs: request failed, OCS %u\n", ocs); return -1; }
    return 0;
}

static uint8_t *upiu(struct ufs *u, uint8_t type, uint8_t flags, uint8_t lun)
{
    uint8_t *p = u->ucd;
    memset(p, 0, 512);
    p[0] = type; p[1] = flags; p[2] = lun; p[3] = u->tag++;
    return p;
}

/* A SCSI command to a LUN; the status from the response (0 good, 2 check condition), or -1. */
static int scsi(struct ufs *u, uint8_t lun, const uint8_t *cdb, int cdb_len, uint32_t len, int dir)
{
    uint8_t *p = upiu(u, UPIU_COMMAND, dir == 2 ? 0x40 : dir == 1 ? 0x20 : 0, lun);
    p[12] = (uint8_t)(len >> 24); p[13] = (uint8_t)(len >> 16); p[14] = (uint8_t)(len >> 8); p[15] = (uint8_t)len;
    memcpy(p + 16, cdb, (size_t)cdb_len);
    if (request(u, len, dir)) return -1;
    uint8_t *r = u->ucd + UCD_RSP_OFF;
    if (r[6]) return -1;                    /* the target failed */
    return r[7];
}

static int lu_rw(struct blkdev *d, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct ufs *u = d->priv;
    uint8_t lun = (uint8_t)(d - u->luns);
    if (write && u->lun_ro[lun]) return -1;
    if (lba + count > d->sectors) return -1;
    lock(u);
    uint32_t per = BOUNCE_PAGES * PAGE_SIZE / d->sector_size;
    int rc = 0;
    while (count && !rc) {
        uint32_t n = count < per ? count : per, bytes = n * d->sector_size;
        uint8_t cdb[16] = { write ? 0x8a : 0x88 };                 /* READ(16) / WRITE(16) */
        for (int i = 0; i < 8; i++) cdb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
        cdb[10] = (uint8_t)(n >> 24); cdb[11] = (uint8_t)(n >> 16); cdb[12] = (uint8_t)(n >> 8); cdb[13] = (uint8_t)n;
        int st = -1;
        for (int t = 0; t < 3 && st != 0; t++) {                   /* a unit attention (check condition) first, maybe */
            if (write) memcpy(u->buf, buf, bytes);
            st = scsi(u, lun, cdb, 16, bytes, write ? 1 : 2);
        }
        if (st != 0) { kprintf("ufs: %s of %u blocks at %llu on LUN %u failed (%d)\n", write ? "write" : "read", n, lba, lun, st); rc = -1; break; }
        if (!write) memcpy(buf, u->buf, bytes);
        buf = (uint8_t *)buf + bytes; lba += n; count -= n;
    }
    unlock(u);
    return rc;
}

static int lu_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf) { return lu_rw(d, lba, count, buf, 0); }
static int lu_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf) { return lu_rw(d, lba, count, (void *)buf, 1); }
static int lu_flush(struct blkdev *d)
{
    struct ufs *u = d->priv;
    uint8_t cdb[10] = { 0x35 };                                     /* SYNCHRONIZE CACHE(10) */
    lock(u);
    int st = scsi(u, (uint8_t)(d - u->luns), cdb, 10, 0, 0);
    unlock(u);
    return st == 0 ? 0 : -1;
}

/* ---- bring-up -------------------------------------------------------------------------- */

static int link_up(struct ufs *u)
{
    wr(u, REG_HCE, 1);
    if (wait_bits(u, REG_HCE, 1, 1, 1000)) { kprintf("ufs: the controller did not enable\n"); return -1; }
    for (int tries = 0; tries < 3; tries++) {
        if (wait_bits(u, REG_HCS, 1u << 3, 1u << 3, 1000)) continue;
        wr(u, REG_IS, IS_UCCS);
        wr(u, REG_UCMDARG1, 0); wr(u, REG_UCMDARG2, 0); wr(u, REG_UCMDARG3, 0);
        wr(u, REG_UICCMD, UIC_LINKSTARTUP);
        if (wait_bits(u, REG_IS, IS_UCCS, IS_UCCS, 1000) == 0 && (rd(u, REG_UCMDARG2) & 0xff) == 0 &&
            wait_bits(u, REG_HCS, 1, 1, 1000) == 0)
            return 0;
    }
    kprintf("ufs: link startup failed (HCS %08x)\n", rd(u, REG_HCS));
    return -1;
}

/* What a bootloader-less start needs: the device answers, and has initialised. */
static int device_init(struct ufs *u)
{
    for (int i = 0; i < 10; i++) {
        upiu(u, UPIU_NOP_OUT, 0, 0);
        if (request(u, 0, 0) == 0 && (u->ucd[UCD_RSP_OFF] & 0x3f) == 0x20) break;   /* NOP IN */
        if (i == 9) { kprintf("ufs: the device does not answer\n"); return -1; }
    }
    uint8_t *p = upiu(u, UPIU_QUERY, 0, 0);
    p[5] = 0x81; p[12] = 0x06; p[13] = 0x01;                         /* write request: SET FLAG fDeviceInit */
    if (request(u, 0, 0)) return -1;
    for (int i = 0; i < 1000; i++) {
        p = upiu(u, UPIU_QUERY, 0, 0);
        p[5] = 0x01; p[12] = 0x05; p[13] = 0x01;                     /* read request: READ FLAG fDeviceInit */
        if (request(u, 0, 0)) return -1;
        if (!(u->ucd[UCD_RSP_OFF + 27] & 1)) return 0;
    }
    kprintf("ufs: fDeviceInit stays set\n");
    return -1;
}

int ufs_attach(volatile void *regs, int phone)
{
    struct ufs *u = kzalloc(sizeof *u);
    if (!u) return -1;
    u->regs = regs;
    uint32_t ver = rd(u, REG_VER), hce = rd(u, REG_HCE) & 1;
    u->keep_running = hce && phone;
    if (phone && !hce) { kprintf("ufs: version %x, left off by the bootloader: its PHY needs bringing up, not done here\n", ver); return -1; }

    u->utrl_phys = pmm_alloc_page();
    u->ucd_phys = pmm_alloc_page();
    u->buf_phys = pmm_alloc_pages(BOUNCE_PAGES);
    if (!u->utrl_phys || !u->ucd_phys || !u->buf_phys) return -1;
    u->utrl = P2V(u->utrl_phys); u->ucd = P2V(u->ucd_phys); u->buf = P2V(u->buf_phys);
    memset(u->utrl, 0, PAGE_SIZE);
    cache_clean(u->utrl, PAGE_SIZE);

    if (!hce && link_up(u)) return -1;
    if (rd(u, REG_UTRLRSR) & 1) {                                     /* the bootloader's list: stop it, take over */
        if (wait_bits(u, REG_UTRLDBR, ~0u, 0, 1000)) { kprintf("ufs: requests still in flight\n"); return -1; }
        wr(u, REG_UTRLRSR, 0);
    }
    if (wait_bits(u, REG_HCS, 1u << 1, 1u << 1, 1000)) { kprintf("ufs: transfer list not ready (HCS %08x)\n", rd(u, REG_HCS)); return -1; }
    wr(u, REG_IE, 0);                                                 /* polled */
    wr(u, REG_UTRLBA, (uint32_t)u->utrl_phys);
    wr(u, REG_UTRLBAU, (uint32_t)(u->utrl_phys >> 32));
    wr(u, REG_UTRLRSR, 1);
    if (!hce && device_init(u)) return -1;

    int found = 0;
    for (uint8_t lun = 0; lun < MAX_LUNS; lun++) {
        uint8_t tur[6] = { 0 }, cap[16] = { 0x9e, 0x10 };             /* TEST UNIT READY, READ CAPACITY(16) */
        cap[13] = 32;
        int st = -1;
        u->probing = 1;
        for (int i = 0; i < 5 && st != 0; i++) st = scsi(u, lun, tur, 6, 0, 0);   /* a unit attention first, maybe */
        if (st == 0) st = scsi(u, lun, cap, 16, 32, 2);
        u->probing = 0;
        if (st != 0) continue;
        uint64_t last = 0;
        for (int i = 0; i < 8; i++) last = last << 8 | u->buf[i];
        uint32_t bs = (uint32_t)u->buf[8] << 24 | (uint32_t)u->buf[9] << 16 | (uint32_t)u->buf[10] << 8 | u->buf[11];
        if (!bs || bs > PAGE_SIZE * BOUNCE_PAGES) continue;
        struct blkdev *d = &u->luns[lun];
        d->name[0] = 's'; d->name[1] = 'd'; d->name[2] = 'a';
        while (blkdev_find(d->name) && d->name[2] < 'z') d->name[2]++;    /* after any SATA disks */
        d->sector_size = bs; d->sectors = last + 1;
        d->read = lu_read; d->write = lu_write; d->flush = lu_flush; d->priv = u;
        u->lun_ro[lun] = phone && lun != 0;
        kprintf("ufs: LUN %u: %llu MiB in %u-byte blocks%s\n", lun, (last + 1) * bs >> 20, bs, u->lun_ro[lun] ? ", read-only" : "");
        blkdev_register(d);
        found++;
    }
    kprintf("ufs: UFSHCI %x.%x, %s, %d logical units\n", ver >> 8 & 0xff, ver >> 4 & 0xf, hce ? "taken over from the bootloader" : "started", found);
    return found ? 0 : -1;
}

void ufs_init(void)
{
    const struct pci_dev *pd;
    for (int i = 0; (pd = pci_find_class(0x01, 0x09, i)); i++) {     /* mass storage: UFS host controller */
        pci_enable_busmaster(pd);
        ufs_attach(vmm_map_mmio(pd->bar[0], 0x1000), 0);
    }
}
