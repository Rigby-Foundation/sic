/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Legacy IDE/ATA in PIO mode (compatibility ports 0x1F0/0x170), LBA48 and
 * LBA28. Slow but universal: BIOS-era machines and IDE-mode chipsets. */
#include "drivers/ide.h"
#include "drivers/pci.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/timer.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

#define REG_DATA    0
#define REG_ERROR   1
#define REG_COUNT   2
#define REG_LBA0    3
#define REG_LBA1    4
#define REG_LBA2    5
#define REG_DRIVE   6
#define REG_STATUS  7
#define REG_CMD     7
#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

struct ide_dev {
    uint16_t base, ctl;
    int slave;
    int lba48;
    spinlock_t lock;
    struct blkdev bdev;
};

static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }

static int wait_ready(uint16_t base, int want_drq)
{
    uint64_t deadline = timer_ticks() + 5000;
    for (;;) {
        uint8_t st = inb(base + REG_STATUS);
        if (st & (ST_ERR | ST_DF)) return -1;
        if (!(st & ST_BSY) && (!want_drq || (st & ST_DRQ))) return 0;
        if (timer_ticks() > deadline) return -1;
    }
}

static void select(struct ide_dev *d)
{
    outb(d->base + REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4)));
    for (int i = 0; i < 4; i++) inb(d->ctl);        /* 400 ns */
}

static int ide_rw(struct blkdev *b, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct ide_dev *d = b->priv;
    uint8_t *p = buf;
    int rc = 0;
    spin_lock(&d->lock);
    while (count && rc == 0) {
        uint32_t n = count < 256 ? count : 256;
        select(d);
        if (d->lba48) {
            outb(d->base + REG_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
            outb(d->base + REG_COUNT, (uint8_t)(n >> 8));
            outb(d->base + REG_LBA0, (uint8_t)(lba >> 24));
            outb(d->base + REG_LBA1, (uint8_t)(lba >> 32));
            outb(d->base + REG_LBA2, (uint8_t)(lba >> 40));
            outb(d->base + REG_COUNT, (uint8_t)n);
            outb(d->base + REG_LBA0, (uint8_t)lba);
            outb(d->base + REG_LBA1, (uint8_t)(lba >> 8));
            outb(d->base + REG_LBA2, (uint8_t)(lba >> 16));
            outb(d->base + REG_CMD, write ? 0x34 : 0x24);       /* WRITE/READ SECTORS EXT */
        } else {
            outb(d->base + REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0xF)));
            outb(d->base + REG_COUNT, (uint8_t)n);
            outb(d->base + REG_LBA0, (uint8_t)lba);
            outb(d->base + REG_LBA1, (uint8_t)(lba >> 8));
            outb(d->base + REG_LBA2, (uint8_t)(lba >> 16));
            outb(d->base + REG_CMD, write ? 0x30 : 0x20);       /* WRITE/READ SECTORS */
        }
        for (uint32_t s = 0; s < n && rc == 0; s++) {
            if (wait_ready(d->base, 1) != 0) { rc = -1; break; }
            uint16_t *w = (uint16_t *)(p + s * 512);
            if (write)
                for (int i = 0; i < 256; i++) outw(d->base + REG_DATA, w[i]);
            else
                for (int i = 0; i < 256; i++) w[i] = inw(d->base + REG_DATA);
        }
        if (write && rc == 0) {
            outb(d->base + REG_CMD, 0xE7);                      /* FLUSH CACHE */
            wait_ready(d->base, 0);
        }
        p += n * 512;
        lba += n;
        count -= n;
    }
    spin_unlock(&d->lock);
    if (rc)
        kprintf("ide: %s: %s error at lba %lu\n", b->name, write ? "write" : "read", lba);
    return rc;
}

static int ide_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)        { return ide_rw(b, lba, count, buf, 0); }
static int ide_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf) { return ide_rw(b, lba, count, (void *)buf, 1); }

static int next_disk;

static void probe(uint16_t base, uint16_t ctl, int slave)
{
    outb(base + REG_DRIVE, (uint8_t)(0xA0 | (slave << 4)));
    for (int i = 0; i < 4; i++) inb(ctl);
    outb(base + REG_COUNT, 0); outb(base + REG_LBA0, 0); outb(base + REG_LBA1, 0); outb(base + REG_LBA2, 0);
    outb(base + REG_CMD, 0xEC);                                 /* IDENTIFY */
    if (inb(base + REG_STATUS) == 0) return;                    /* no drive */
    uint64_t deadline = timer_ticks() + 1000;
    for (;;) {
        uint8_t st = inb(base + REG_STATUS);
        if (st & ST_ERR) return;                                /* ATAPI or nothing */
        if (!(st & ST_BSY) && (st & ST_DRQ)) break;
        if (timer_ticks() > deadline) return;
    }
    if (inb(base + REG_LBA1) || inb(base + REG_LBA2)) return;   /* not ATA */

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(base + REG_DATA);
    struct ide_dev *d = kzalloc(sizeof(*d));
    if (!d) return;
    d->base = base; d->ctl = ctl; d->slave = slave;
    d->lba48 = (id[83] & (1 << 10)) != 0;
    uint64_t sectors = 0;
    if (d->lba48) memcpy(&sectors, id + 100, 8);
    if (!sectors) sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    char model[41];
    for (int i = 0; i < 20; i++) { model[i * 2] = (char)(id[27 + i] >> 8); model[i * 2 + 1] = (char)id[27 + i]; }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    d->bdev.name[0] = 'h'; d->bdev.name[1] = 'd'; d->bdev.name[2] = (char)('a' + next_disk++); d->bdev.name[3] = 0;
    d->bdev.sector_size = 512;
    d->bdev.sectors = sectors;
    d->bdev.read = ide_read;
    d->bdev.write = ide_write;
    d->bdev.priv = d;
    kprintf("ide: %s: %s (%s)\n", d->bdev.name, model, d->lba48 ? "lba48" : "lba28");
    blkdev_register(&d->bdev);
}

void ide_init(void)
{
    /* Only worth probing when a PCI IDE controller exists (or on very old
     * machines, but they also have one). Compatibility-mode ports assumed. */
    if (!pci_find_class(0x01, 0x01, 0)) return;
    probe(0x1F0, 0x3F6, 0);
    probe(0x1F0, 0x3F6, 1);
    probe(0x170, 0x376, 0);
    probe(0x170, 0x376, 1);
}
