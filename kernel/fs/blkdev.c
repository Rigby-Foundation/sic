/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "fs/blkdev.h"
#include "endian.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

static struct blkdev *devices;

/* Whole sectors go to the driver in runs; partial ones through a bounce sector. */
long blkdev_read_bytes(struct blkdev *d, uint64_t off, void *buf, size_t len)
{
    uint64_t total = d->sectors * d->sector_size;
    if (off >= total)
        return 0;
    if (off + len > total)
        len = (size_t)(total - off);
    uint8_t *out = buf;
    size_t done = 0;
    uint8_t *bounce = kmalloc(d->sector_size);
    if (!bounce)
        return -1;
    while (done < len) {
        uint64_t lba = (off + done) / d->sector_size;
        size_t   in  = (size_t)((off + done) % d->sector_size);
        size_t   n   = d->sector_size - in;
        if (n > len - done)
            n = len - done;
        if (in == 0 && n == d->sector_size) {
            uint32_t k = (uint32_t)((len - done) / d->sector_size);   /* every whole sector from here, at once */
            if (k > 2048) k = 2048;
            if (d->read(d, lba, k, out + done) != 0) break;
            n = (size_t)k * d->sector_size;
        } else {
            if (d->read(d, lba, 1, bounce) != 0) break;
            memcpy(out + done, bounce + in, n);
        }
        done += n;
    }
    kfree(bounce);
    return (long)done;
}

long blkdev_write_bytes(struct blkdev *d, uint64_t off, const void *buf, size_t len)
{
    uint64_t total = d->sectors * d->sector_size;
    if (off >= total)
        return -1;
    if (off + len > total)
        len = (size_t)(total - off);
    const uint8_t *in = buf;
    size_t done = 0;
    uint8_t *bounce = kmalloc(d->sector_size);
    if (!bounce)
        return -1;
    while (done < len) {
        uint64_t lba = (off + done) / d->sector_size;
        size_t   at  = (size_t)((off + done) % d->sector_size);
        size_t   n   = d->sector_size - at;
        if (n > len - done)
            n = len - done;
        if (at == 0 && n == d->sector_size) {
            uint32_t k = (uint32_t)((len - done) / d->sector_size);
            if (k > 2048) k = 2048;
            if (d->write(d, lba, k, in + done) != 0) break;
            n = (size_t)k * d->sector_size;
        } else {
            if (d->read(d, lba, 1, bounce) != 0) break;
            memcpy(bounce + at, in + done, n);
            if (d->write(d, lba, 1, bounce) != 0) break;
        }
        done += n;
    }
    kfree(bounce);
    return (long)done;
}

static long node_read(struct file *f, void *buf, size_t len)
{
    long r = blkdev_read_bytes(f->node->priv, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    return r;
}

static long node_write(struct file *f, const void *buf, size_t len)
{
    long r = blkdev_write_bytes(f->node->priv, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    return r;
}

static const struct dev_ops blk_node_ops = { .read = node_read, .write = node_write };

/* ---- partitions ------------------------------------------------------------ */

static int part_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > d->sectors) return -1;
    return d->parent->read(d->parent, d->start + lba, count, buf);
}

static int part_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (lba + count > d->sectors) return -1;
    return d->parent->write(d->parent, d->start + lba, count, buf);
}

/* A GPT partition's name, as /dev/by-name/<label> (a phone's "userdata"). */
static void name_partition(struct blkdev *p, const uint16_t *label)
{
    char path[64] = "/dev/by-name/";
    size_t n = strlen(path), start = n;
    for (int i = 0; i < 36 && label[i] && n < sizeof path - 1; i++) {
        uint16_t c = le16toh(label[i]);
        path[n++] = c > 32 && c < 127 && c != '/' ? (char)c : '_';
    }
    path[n] = 0;
    if (n == start) return;
    if (!vfs_lookup(vfs_root(), "/dev/by-name")) vfs_create(vfs_root(), "/dev/by-name", VNODE_DIR);
    if (vfs_lookup(vfs_root(), path)) return;              /* two LUNs with the same label: the first keeps it */
    vfs_mkdev(path, &blk_node_ops, p);
    struct vnode *v = vfs_lookup(vfs_root(), path);
    if (v) { v->size = p->sectors * p->sector_size; v->is_block = 1; }
}

static struct blkdev *add_partition(struct blkdev *disk, int index, uint64_t start, uint64_t sectors)
{
    if (!sectors || start + sectors > disk->sectors)
        return NULL;
    char name[16];
    size_t n = strlen(disk->name);
    memcpy(name, disk->name, n);
    /* nvme0n1 -> nvme0n1p1, sda -> sda1, sda12 */
    if (disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9')
        name[n++] = 'p';
    if (index >= 100) name[n++] = (char)('0' + index / 100);
    if (index >= 10) name[n++] = (char)('0' + index / 10 % 10);
    name[n++] = (char)('0' + index % 10);
    name[n] = 0;
    struct blkdev *old = blkdev_find(name);
    if (old) {                          /* rescan: update in place */
        old->start = start;
        old->sectors = sectors;
        char path[32] = "/dev/";
        memcpy(path + 5, name, n + 1);
        struct vnode *v = vfs_lookup(vfs_root(), path);
        if (v) v->size = sectors * disk->sector_size;
        return old;
    }
    struct blkdev *p = kzalloc(sizeof(*p));
    if (!p) return NULL;
    memcpy(p->name, name, n + 1);
    p->sector_size = disk->sector_size;
    p->sectors = sectors;
    p->parent = disk;
    p->start = start;
    p->read = part_read;
    p->write = part_write;
    p->priv = disk->priv;
    blkdev_register(p);
    return p;
}

struct gpt_header {
    char     sig[8];
    uint32_t revision, header_size, crc, reserved;
    uint64_t current_lba, backup_lba, first_usable, last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t  type[16], guid[16];
    uint64_t first, last, attrs;
    uint16_t name[36];
} __attribute__((packed));

static void scan_partitions(struct blkdev *disk)
{
    uint32_t ss = disk->sector_size;                      /* 512, or 4096 on UFS */
    if ((ss != 512 && ss != 4096) || disk->sectors < 2)
        return;
    uint8_t *sec = kmalloc(ss);
    if (!sec) return;
    if (disk->read(disk, 0, 1, sec) != 0) { kprintf("blk: %s: sector 0 does not read\n", disk->name); goto out; }
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        kprintf("blk: %s: no partition table (sector 0 ends %02x%02x, starts %02x%02x%02x%02x)\n", disk->name, sec[510], sec[511], sec[0], sec[1], sec[2], sec[3]);
        goto out;
    }

    /* GPT: a protective MBR (type 0xEE) and a valid header at LBA 1. */
    int gpt = 0;
    for (int i = 0; i < 4; i++)
        if (sec[446 + i * 16 + 4] == 0xEE) gpt = 1;
    if (gpt) {
        struct gpt_header h;
        if (disk->read(disk, 1, 1, sec) != 0) { kprintf("blk: %s: the GPT header does not read\n", disk->name); goto out; }
        memcpy(&h, sec, sizeof(h));
        uint32_t entry_size = le32toh(h.entry_size), entry_count = le32toh(h.entry_count);
        uint64_t entries_lba = le64toh(h.entries_lba);
        if (memcmp(h.sig, "EFI PART", 8) != 0 || entry_size < sizeof(struct gpt_entry)) {
            kprintf("blk: %s: protective MBR, but no GPT header at sector 1 (entry size %u)\n", disk->name, entry_size);
            goto out;
        }
        uint32_t per_sector = ss / entry_size;
        int index = 1;
        for (uint32_t i = 0; i < entry_count && i < 128; i++) {
            if (i % per_sector == 0 && disk->read(disk, entries_lba + i / per_sector, 1, sec) != 0) {
                kprintf("blk: %s: GPT entries at sector %llu do not read\n", disk->name, entries_lba + i / per_sector);
                break;
            }
            struct gpt_entry *e = (void *)(sec + (i % per_sector) * entry_size);
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e->type[k]) empty = 0;
            if (empty) { index++; continue; }
            uint16_t label[36];
            memcpy(label, e->name, sizeof label);
            struct blkdev *p = add_partition(disk, index++, le64toh(e->first), le64toh(e->last) - le64toh(e->first) + 1);
            if (p) name_partition(p, label);
        }
        goto out;
    }
    /* MBR: primary partitions only. */
    for (int i = 0; i < 4; i++) {
        uint8_t *e = sec + 446 + i * 16;
        uint32_t start = get_le32(e + 8), count = get_le32(e + 12);
        if (e[4] && count)
            add_partition(disk, i + 1, start, count);
    }
out:
    kfree(sec);
}

void blkdev_rescan(struct blkdev *disk)
{
    if (!disk->parent)
        scan_partitions(disk);
}

void blkdev_register(struct blkdev *d)
{
    d->next = devices;
    devices = d;
    char path[32] = "/dev/";
    memcpy(path + 5, d->name, strlen(d->name) + 1);
    vfs_mkdev(path, &blk_node_ops, d);
    struct vnode *n = vfs_lookup(vfs_root(), path);
    if (n) {
        n->size = d->sectors * d->sector_size;
        n->is_block = 1;
    }
    if (d->parent)
        kprintf("blk:   %s: %llu MiB at sector %llu\n", d->name, d->sectors * d->sector_size >> 20, d->start);
    else
        kprintf("blk: %s: %llu MiB (%llu x %u)\n", d->name,
                d->sectors * d->sector_size >> 20, d->sectors, d->sector_size);
    if (!d->parent)
        scan_partitions(d);
}

struct blkdev *blkdev_find(const char *name)
{
    for (struct blkdev *d = devices; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

void blkdev_flush_all(void)
{
    for (struct blkdev *d = devices; d; d = d->next)
        if (!d->parent && d->flush)
            d->flush(d);
}

struct blkdev *blkdev_from_vnode(struct vnode *n)
{
    if (!n || n->type != VNODE_DEV || n->dev != &blk_node_ops)
        return NULL;
    return n->priv;
}
