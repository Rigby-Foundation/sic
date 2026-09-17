/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* IDE/ATA in PIO mode, LBA48 and LBA28. Slow but universal: the legacy
 * compatibility ports (0x1F0/0x170) of BIOS-era PCs and IDE-mode chipsets,
 * and the memory-mapped ATA cells in a PowerMac's mac-io (the same taskfile,
 * registers 16 bytes apart, control register at +0x160). */
#include "drivers/ide.h"
#include "endian.h"
#include "drivers/pci.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#ifdef __x86_64__
#include "asm/io.h"
#endif
#ifdef __powerpc__
#include "asm/of.h"
#endif

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

/* One channel: either I/O ports (base/ctl) or a memory-mapped cell. */
struct ide_chan {
    uint16_t base, ctl;                 /* port I/O (x86) */
    volatile uint8_t *mmio;             /* mac-io cell (powerpc); registers at mmio + reg * 16 */
};

struct ide_dev {
    struct ide_chan ch;
    int slave;
    int lba48;
    spinlock_t lock;
    struct blkdev bdev;
};

static uint8_t rd8(const struct ide_chan *c, int reg)
{
#ifdef __x86_64__
    if (!c->mmio) return inb(c->base + reg);
#endif
    return mmio_read8(c->mmio + reg * 16);
}

static void wr8(const struct ide_chan *c, int reg, uint8_t v)
{
#ifdef __x86_64__
    if (!c->mmio) { outb(c->base + reg, v); return; }
#endif
    mmio_write8(c->mmio + reg * 16, v);
}

/* The data register is a little-endian 16-bit port: reads give the sector's
 * bytes in order once stored little-endian (see ide_rw). */
static uint16_t rd16(const struct ide_chan *c)
{
#ifdef __x86_64__
    if (!c->mmio) return inw(c->base + REG_DATA);
#endif
    return mmio_read16(c->mmio);
}

static void wr16(const struct ide_chan *c, uint16_t v)
{
#ifdef __x86_64__
    if (!c->mmio) { outw(c->base + REG_DATA, v); return; }
#endif
    mmio_write16(c->mmio, v);
}

static uint8_t rd_ctl(const struct ide_chan *c)
{
#ifdef __x86_64__
    if (!c->mmio) return inb(c->ctl);
#endif
    return mmio_read8(c->mmio + 0x160);
}

static int wait_ready(const struct ide_chan *c, int want_drq)
{
    uint64_t deadline = timer_ticks() + 5000;
    for (;;) {
        uint8_t st = rd8(c, REG_STATUS);
        if (st & (ST_ERR | ST_DF)) return -1;
        if (!(st & ST_BSY) && (!want_drq || (st & ST_DRQ))) return 0;
        if (timer_ticks() > deadline) return -1;
    }
}

static void select(struct ide_dev *d)
{
    wr8(&d->ch, REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4)));
    for (int i = 0; i < 4; i++) rd_ctl(&d->ch);        /* 400 ns */
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
            wr8(&d->ch, REG_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
            wr8(&d->ch, REG_COUNT, (uint8_t)(n >> 8));
            wr8(&d->ch, REG_LBA0, (uint8_t)(lba >> 24));
            wr8(&d->ch, REG_LBA1, (uint8_t)(lba >> 32));
            wr8(&d->ch, REG_LBA2, (uint8_t)(lba >> 40));
            wr8(&d->ch, REG_COUNT, (uint8_t)n);
            wr8(&d->ch, REG_LBA0, (uint8_t)lba);
            wr8(&d->ch, REG_LBA1, (uint8_t)(lba >> 8));
            wr8(&d->ch, REG_LBA2, (uint8_t)(lba >> 16));
            wr8(&d->ch, REG_CMD, write ? 0x34 : 0x24);       /* WRITE/READ SECTORS EXT */
        } else {
            wr8(&d->ch, REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0xF)));
            wr8(&d->ch, REG_COUNT, (uint8_t)n);
            wr8(&d->ch, REG_LBA0, (uint8_t)lba);
            wr8(&d->ch, REG_LBA1, (uint8_t)(lba >> 8));
            wr8(&d->ch, REG_LBA2, (uint8_t)(lba >> 16));
            wr8(&d->ch, REG_CMD, write ? 0x30 : 0x20);       /* WRITE/READ SECTORS */
        }
        for (uint32_t s = 0; s < n && rc == 0; s++) {
            if (wait_ready(&d->ch, 1) != 0) { rc = -1; break; }
            uint16_t *w = (uint16_t *)(p + s * 512);
            if (write)
                for (int i = 0; i < 256; i++) wr16(&d->ch, le16toh(w[i]));   /* sector bytes in order */
            else
                for (int i = 0; i < 256; i++) w[i] = htole16(rd16(&d->ch));
        }
        if (write && rc == 0) {
            wr8(&d->ch, REG_CMD, 0xE7);                      /* FLUSH CACHE */
            wait_ready(&d->ch, 0);
        }
        p += n * 512;
        lba += n;
        count -= n;
    }
    spin_unlock(&d->lock);
    if (rc)
        kprintf("ide: %s: %s error at lba %llu\n", b->name, write ? "write" : "read", lba);
    return rc;
}

static int ide_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)        { return ide_rw(b, lba, count, buf, 0); }
static int ide_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf) { return ide_rw(b, lba, count, (void *)buf, 1); }

static int next_disk;

static void probe(const struct ide_chan *c, int slave)
{
    wr8(c, REG_DRIVE, (uint8_t)(0xA0 | (slave << 4)));
    for (int i = 0; i < 4; i++) rd_ctl(c);
    wr8(c, REG_COUNT, 0); wr8(c, REG_LBA0, 0); wr8(c, REG_LBA1, 0); wr8(c, REG_LBA2, 0);
    wr8(c, REG_CMD, 0xEC);                                      /* IDENTIFY */
    uint8_t st0 = rd8(c, REG_STATUS);
    if (st0 == 0 || st0 == 0xFF) return;                        /* no drive (0xFF: nothing on the bus) */
    uint64_t deadline = timer_ticks() + 1000;
    for (;;) {
        uint8_t st = rd8(c, REG_STATUS);
        if (st & ST_ERR) return;                                /* ATAPI or nothing */
        if (!(st & ST_BSY) && (st & ST_DRQ)) break;
        if (timer_ticks() > deadline) return;
    }
    if (rd8(c, REG_LBA1) || rd8(c, REG_LBA2)) return;           /* not ATA */

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = rd16(c);
    struct ide_dev *d = kzalloc(sizeof(*d));
    if (!d) return;
    d->ch = *c; d->slave = slave;
    d->lba48 = (id[83] & (1 << 10)) != 0;
    uint64_t sectors = 0;
    if (d->lba48) sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
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
#ifdef __x86_64__
    /* Only worth probing when a PCI IDE controller exists (or on very old
     * machines, but they also have one). Compatibility-mode ports assumed. */
    if (!pci_find_class(0x01, 0x01, 0)) return;
    static const struct ide_chan chans[2] = { { 0x1F0, 0x3F6, NULL }, { 0x170, 0x376, NULL } };
    for (int i = 0; i < 2; i++) {
        probe(&chans[i], 0);
        probe(&chans[i], 1);
    }
#endif
#ifdef __powerpc__
    /* The ATA cells the firmware listed under mac-io. */
    for (int i = 0; i < platform.ide_count; i++) {
        struct ide_chan c = { 0, 0, vmm_map_mmio(platform.macio_base + platform.ide_offset[i], 0x1000) };
        if (!c.mmio) continue;
        probe(&c, 0);
        probe(&c, 1);
    }
#endif
}
