/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "fs/blkdev.h"
#include "endian.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

static struct blkdev *devices;

/* Partial-sector transfers go through a bounce sector. */
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
            if (d->read(d, lba, 1, out + done) != 0) break;
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
            if (d->write(d, lba, 1, in + done) != 0) break;
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

static void add_partition(struct blkdev *disk, int index, uint64_t start, uint64_t sectors)
{
    if (!sectors || start + sectors > disk->sectors)
        return;
    char name[16];
    size_t n = strlen(disk->name);
    memcpy(name, disk->name, n);
    /* nvme0n1 -> nvme0n1p1, sda -> sda1 */
    if (disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9')
        name[n++] = 'p';
    name[n++] = (char)('0' + index);
    name[n] = 0;
    struct blkdev *old = blkdev_find(name);
    if (old) {                          /* rescan: update in place */
        old->start = start;
        old->sectors = sectors;
        char path[32] = "/dev/";
        memcpy(path + 5, name, n + 1);
        struct vnode *v = vfs_lookup(vfs_root(), path);
        if (v) v->size = sectors * disk->sector_size;
        return;
    }
    struct blkdev *p = kzalloc(sizeof(*p));
    if (!p) return;
    memcpy(p->name, name, n + 1);
    p->sector_size = disk->sector_size;
    p->sectors = sectors;
    p->parent = disk;
    p->start = start;
    p->read = part_read;
    p->write = part_write;
    p->priv = disk->priv;
    blkdev_register(p);
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
    if (disk->sector_size != 512 || disk->sectors < 2)
        return;
    uint8_t *sec = kmalloc(512);
    if (!sec) return;
    if (disk->read(disk, 0, 1, sec) != 0 || sec[510] != 0x55 || sec[511] != 0xAA)
        goto out;

    /* GPT: a protective MBR (type 0xEE) and a valid header at LBA 1. */
    int gpt = 0;
    for (int i = 0; i < 4; i++)
        if (sec[446 + i * 16 + 4] == 0xEE) gpt = 1;
    if (gpt) {
        struct gpt_header h;
        if (disk->read(disk, 1, 1, sec) != 0) goto out;
        memcpy(&h, sec, sizeof(h));
        uint32_t entry_size = le32toh(h.entry_size), entry_count = le32toh(h.entry_count);
        uint64_t entries_lba = le64toh(h.entries_lba);
        if (memcmp(h.sig, "EFI PART", 8) != 0 || entry_size < sizeof(struct gpt_entry)) goto out;
        uint32_t per_sector = 512 / entry_size;
        int index = 1;
        for (uint32_t i = 0; i < entry_count && i < 128; i++) {
            if (i % per_sector == 0 && disk->read(disk, entries_lba + i / per_sector, 1, sec) != 0) break;
            struct gpt_entry *e = (void *)(sec + (i % per_sector) * entry_size);
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e->type[k]) empty = 0;
            if (empty) { index++; continue; }
            add_partition(disk, index++, le64toh(e->first), le64toh(e->last) - le64toh(e->first) + 1);
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

struct blkdev *blkdev_from_vnode(struct vnode *n)
{
    if (!n || n->type != VNODE_DEV || n->dev != &blk_node_ops)
        return NULL;
    return n->priv;
}
