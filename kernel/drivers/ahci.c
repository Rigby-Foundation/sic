/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* AHCI (SATA): polled, one command slot, READ/WRITE DMA EXT through a
 * bounce buffer of one page per transfer. Registers /dev/sdX per port. */
#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "fs/blkdev.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "arch/x86_64/timer.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_PI     0x0C
#define HBA_PORTS  0x100
#define PORT_SIZE  0x80

#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1 << 0)
#define CMD_FRE  (1 << 4)
#define CMD_FR   (1 << 14)
#define CMD_CR   (1 << 15)
#define GHC_AE   (1u << 31)
#define TFD_BSY  0x80
#define TFD_DRQ  0x08
#define SIG_SATA 0x00000101

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35

struct fis_h2d {
    uint8_t  type;          /* 0x27 */
    uint8_t  flags;         /* bit 7: command */
    uint8_t  command;
    uint8_t  feature_lo;
    uint8_t  lba0, lba1, lba2, device;
    uint8_t  lba3, lba4, lba5, feature_hi;
    uint8_t  count_lo, count_hi, icc, control;
    uint8_t  reserved[4];
} __attribute__((packed));

struct cmd_header {
    uint16_t flags;         /* bits 0-4: FIS length in dwords, bit 6: write, bit 10: clear busy */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

struct prdt_entry {
    uint32_t dba, dbau;
    uint32_t reserved;
    uint32_t dbc;           /* byte count - 1, bit 31: interrupt */
} __attribute__((packed));

struct cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct prdt_entry prdt[1];
} __attribute__((packed));

struct ahci_port {
    volatile uint8_t *regs;
    uint64_t clb_phys, fb_phys, ct_phys, buf_phys;
    spinlock_t lock;
    struct blkdev bdev;
};

static inline uint32_t rd(volatile uint8_t *base, uint32_t r) { return *(volatile uint32_t *)(base + r); }
static inline void wr(volatile uint8_t *base, uint32_t r, uint32_t v) { *(volatile uint32_t *)(base + r) = v; }

static int wait_clear(volatile uint8_t *p, uint32_t r, uint32_t mask, uint64_t ms)
{
    uint64_t deadline = timer_ticks() + ms;
    while (rd(p, r) & mask)
        if (timer_ticks() > deadline)
            return -1;
    return 0;
}

static void port_stop(volatile uint8_t *p)
{
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR, 500);
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR, 500);
}

static void port_start(volatile uint8_t *p)
{
    wait_clear(p, PX_CMD, CMD_CR, 500);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
}

/* Issue one command with `bytes` of data in the bounce page; returns 0 on success. */
static int issue(struct ahci_port *ap, uint8_t command, uint64_t lba, uint16_t count, uint32_t bytes, int write)
{
    volatile uint8_t *p = ap->regs;
    struct cmd_header *hdr = P2V(ap->clb_phys);
    struct cmd_table *ct = P2V(ap->ct_phys);

    memset(ct, 0, sizeof(*ct));
    struct fis_h2d *fis = (void *)ct->cfis;
    fis->type = 0x27;
    fis->flags = 0x80;
    fis->command = command;
    fis->device = 1 << 6;                     /* LBA mode */
    fis->lba0 = (uint8_t)lba;  fis->lba1 = (uint8_t)(lba >> 8);  fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24); fis->lba4 = (uint8_t)(lba >> 32); fis->lba5 = (uint8_t)(lba >> 40);
    fis->count_lo = (uint8_t)count;
    fis->count_hi = (uint8_t)(count >> 8);
    if (bytes) {
        ct->prdt[0].dba = (uint32_t)ap->buf_phys;
        ct->prdt[0].dbau = (uint32_t)(ap->buf_phys >> 32);
        ct->prdt[0].dbc = bytes - 1;
    }
    hdr[0].flags = (uint16_t)((sizeof(*fis) / 4) | (write ? (1 << 6) : 0));
    hdr[0].prdtl = bytes ? 1 : 0;
    hdr[0].prdbc = 0;

    if (wait_clear(p, PX_TFD, TFD_BSY | TFD_DRQ, 1000) != 0)
        return -1;
    wr(p, PX_IS, 0xFFFFFFFF);
    wr(p, PX_CI, 1);
    uint64_t deadline = timer_ticks() + 5000;
    while (rd(p, PX_CI) & 1) {
        if (rd(p, PX_IS) & (1u << 30)) return -1;       /* task file error */
        if (timer_ticks() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return (rd(p, PX_TFD) & 1) ? -1 : 0;              /* ERR bit */
}

static int ahci_rw(struct blkdev *d, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct ahci_port *ap = d->priv;
    uint8_t *b = buf;
    int rc = 0;
    spin_lock(&ap->lock);
    while (count && rc == 0) {
        uint32_t n = count < 8 ? count : 8;           /* 8 x 512 = one page */
        if (write)
            memcpy(P2V(ap->buf_phys), b, n * 512);
        rc = issue(ap, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, (uint16_t)n, n * 512, write);
        if (rc == 0 && !write)
            memcpy(b, P2V(ap->buf_phys), n * 512);
        b += n * 512;
        lba += n;
        count -= n;
    }
    spin_unlock(&ap->lock);
    if (rc)
        kprintf("ahci: %s: %s error at lba %lu\n", d->name, write ? "write" : "read", lba);
    return rc;
}

static int ahci_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)        { return ahci_rw(d, lba, count, buf, 0); }
static int ahci_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf) { return ahci_rw(d, lba, count, (void *)buf, 1); }

static void ata_string(char *dst, const uint16_t *src, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i * 2] = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)src[i];
    }
    dst[words * 2] = 0;
    for (int i = words * 2 - 1; i >= 0 && (dst[i] == ' ' || dst[i] == 0); i--) dst[i] = 0;
}

static int next_disk;

static void port_init(volatile uint8_t *hba, int idx)
{
    volatile uint8_t *p = hba + HBA_PORTS + idx * PORT_SIZE;
    uint32_t ssts = rd(p, PX_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)   /* device present, active */
        return;
    if (rd(p, PX_SIG) != SIG_SATA)
        return;                                          /* ATAPI etc.: not a disk */

    struct ahci_port *ap = kzalloc(sizeof(*ap));
    if (!ap) return;
    ap->regs = p;
    ap->clb_phys = pmm_alloc_page();
    ap->fb_phys  = pmm_alloc_page();
    ap->ct_phys  = pmm_alloc_page();
    ap->buf_phys = pmm_alloc_page();
    memset(P2V(ap->clb_phys), 0, PAGE_SIZE);
    memset(P2V(ap->fb_phys), 0, PAGE_SIZE);

    port_stop(p);
    wr(p, PX_CLB, (uint32_t)ap->clb_phys);  wr(p, PX_CLBU, (uint32_t)(ap->clb_phys >> 32));
    wr(p, PX_FB, (uint32_t)ap->fb_phys);    wr(p, PX_FBU, (uint32_t)(ap->fb_phys >> 32));
    struct cmd_header *hdr = P2V(ap->clb_phys);
    hdr[0].ctba = (uint32_t)ap->ct_phys;
    hdr[0].ctbau = (uint32_t)(ap->ct_phys >> 32);
    wr(p, PX_SERR, 0xFFFFFFFF);
    wr(p, PX_IE, 0);
    port_start(p);

    if (issue(ap, ATA_IDENTIFY, 0, 0, 512, 0) != 0) {
        kprintf("ahci: port %d: identify failed\n", idx);
        return;
    }
    const uint16_t *id = P2V(ap->buf_phys);
    uint64_t sectors;
    memcpy(&sectors, id + 100, 8);                      /* words 100-103: LBA48 count */
    if (!sectors) sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    char model[41];
    ata_string(model, id + 27, 20);

    ap->bdev.name[0] = 's'; ap->bdev.name[1] = 'd'; ap->bdev.name[2] = (char)('a' + next_disk++); ap->bdev.name[3] = 0;
    ap->bdev.sector_size = 512;
    ap->bdev.sectors = sectors;
    ap->bdev.read = ahci_read;
    ap->bdev.write = ahci_write;
    ap->bdev.priv = ap;
    kprintf("ahci: %s: port %d, %s\n", ap->bdev.name, idx, model);
    blkdev_register(&ap->bdev);
}

void ahci_init(void)
{
    const struct pci_dev *pd;
    for (int i = 0; (pd = pci_find_class(0x01, 0x06, i)); i++) {
        if (pd->prog_if != 0x01) continue;
        pci_enable_busmaster(pd);
        volatile uint8_t *hba = vmm_map_mmio(pd->bar[5], 0x1100);
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
        uint32_t pi = rd(hba, HBA_PI);
        kprintf("ahci: controller %04x:%04x, ports %x\n", pd->vendor, pd->device, pi);
        for (int port = 0; port < 32; port++)
            if (pi & (1u << port))
                port_init(hba, port);
    }
}
