/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* zaefs: sic's own on-disk filesystem (see include/abi/zaefs.h). */
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"
#include "arch/x86_64/timer.h"
#include "abi/zaefs.h"

#define BS ZAEFS_BLOCK_SIZE
#define CACHE_SLOTS 64

struct cache_slot {
    uint64_t block;
    int valid;
    uint8_t data[BS];
};

struct zaefs {
    struct blkdev *dev;
    struct zaefs_superblock sb;
    uint32_t sectors_per_block;
    struct cache_slot *cache;
    uint64_t block_hint, inode_hint;
};

/* Per-vnode private data: the in-memory copy of the inode. */
struct zinode {
    uint32_t ino;
    struct zaefs_inode di;
    int dirty;
};

/* ---- block cache (write-through) --------------------------------------------- */

static struct cache_slot *slot_for(struct zaefs *fs, uint64_t block)
{
    return &fs->cache[block % CACHE_SLOTS];
}

static int read_block(struct zaefs *fs, uint64_t block, void *buf)
{
    struct cache_slot *s = slot_for(fs, block);
    if (!(s->valid && s->block == block)) {
        if (fs->dev->read(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, s->data) != 0)
            return -1;
        s->block = block;
        s->valid = 1;
    }
    memcpy(buf, s->data, BS);
    return 0;
}

static int write_block(struct zaefs *fs, uint64_t block, const void *buf)
{
    struct cache_slot *s = slot_for(fs, block);
    memcpy(s->data, buf, BS);
    s->block = block;
    s->valid = 1;
    return fs->dev->write(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, buf);
}

static int write_sb(struct zaefs *fs)
{
    return write_block(fs, 0, &fs->sb);
}

/* ---- bitmaps ----------------------------------------------------------------------- */

/* Scratch blocks come from the heap: kernel stacks are not that big. */
static int64_t bitmap_alloc(struct zaefs *fs, uint64_t start, uint64_t nblocks, uint64_t count, uint64_t *hint)
{
    uint8_t *buf = kmalloc(BS);
    int64_t found = -1;
    if (!buf) return -1;
    for (uint64_t pass = 0; pass < 2 && found < 0; pass++) {
        uint64_t from = pass == 0 ? *hint : 0;
        for (uint64_t i = from; i < count; i++) {
            uint64_t b = start + i / (BS * 8), bit = i % (BS * 8);
            if (bit == 0 || i == from)
                if (read_block(fs, b, buf) != 0) goto out;
            if (!(buf[bit / 8] & (1 << (bit % 8)))) {
                buf[bit / 8] |= (uint8_t)(1 << (bit % 8));
                if (write_block(fs, b, buf) != 0) goto out;
                *hint = i + 1;
                found = (int64_t)i;
                break;
            }
        }
    }
out:
    (void)nblocks;
    kfree(buf);
    return found;
}

static int bitmap_free(struct zaefs *fs, uint64_t start, uint64_t i, uint64_t *hint)
{
    uint8_t *buf = kmalloc(BS);
    if (!buf) return -1;
    uint64_t b = start + i / (BS * 8), bit = i % (BS * 8);
    int rc = read_block(fs, b, buf);
    if (rc == 0) {
        buf[bit / 8] &= (uint8_t)~(1 << (bit % 8));
        if (i < *hint) *hint = i;
        rc = write_block(fs, b, buf);
    }
    kfree(buf);
    return rc;
}

static int64_t alloc_block(struct zaefs *fs)
{
    int64_t b = bitmap_alloc(fs, fs->sb.bbitmap_start, fs->sb.bbitmap_blocks, fs->sb.total_blocks, &fs->block_hint);
    if (b < 0) return -1;
    uint8_t *zero = kzalloc(BS);
    if (!zero) return -1;
    write_block(fs, (uint64_t)b, zero);
    kfree(zero);
    fs->sb.free_blocks--;
    write_sb(fs);
    return b;
}

static void free_block(struct zaefs *fs, uint64_t b)
{
    if (b < fs->sb.data_start || b >= fs->sb.total_blocks) return;
    bitmap_free(fs, fs->sb.bbitmap_start, b, &fs->block_hint);
    fs->sb.free_blocks++;
    write_sb(fs);
}

static int64_t alloc_inode(struct zaefs *fs)
{
    int64_t i = bitmap_alloc(fs, fs->sb.ibitmap_start, fs->sb.ibitmap_blocks, fs->sb.inode_count, &fs->inode_hint);
    if (i <= 0) return -1;              /* inode 0 is never handed out */
    fs->sb.free_inodes--;
    write_sb(fs);
    return i;
}

static void free_inode(struct zaefs *fs, uint64_t i)
{
    bitmap_free(fs, fs->sb.ibitmap_start, i, &fs->inode_hint);
    fs->sb.free_inodes++;
    write_sb(fs);
}

/* ---- inodes ------------------------------------------------------------------------- */

static int inode_read(struct zaefs *fs, uint32_t ino, struct zaefs_inode *out)
{
    uint8_t *buf = kmalloc(BS);
    if (!buf) return -1;
    uint64_t off = (uint64_t)ino * ZAEFS_INODE_SIZE;
    int rc = read_block(fs, fs->sb.itable_start + off / BS, buf);
    if (rc == 0)
        memcpy(out, buf + off % BS, sizeof(*out));
    kfree(buf);
    return rc;
}

static int inode_write(struct zaefs *fs, uint32_t ino, const struct zaefs_inode *in)
{
    uint8_t *buf = kmalloc(BS);
    if (!buf) return -1;
    uint64_t off = (uint64_t)ino * ZAEFS_INODE_SIZE;
    int rc = read_block(fs, fs->sb.itable_start + off / BS, buf);
    if (rc == 0) {
        memcpy(buf + off % BS, in, sizeof(*in));
        rc = write_block(fs, fs->sb.itable_start + off / BS, buf);
    }
    kfree(buf);
    return rc;
}

static struct zaefs *fs_of(struct vnode *n) { return n->mnt->priv; }
static struct zinode *zi_of(struct vnode *n) { return n->priv; }

static int zi_flush(struct vnode *n)
{
    struct zinode *zi = zi_of(n);
    if (!zi || !zi->dirty) return 0;
    zi->dirty = 0;
    return inode_write(fs_of(n), zi->ino, &zi->di);
}

/* Logical block -> physical block, allocating index/data blocks if asked. */
static int64_t bmap_indirect(struct zaefs *fs, struct zinode *zi, uint64_t lblk, int alloc, uint32_t *ptrs);

static int64_t bmap(struct zaefs *fs, struct zinode *zi, uint64_t lblk, int alloc)
{
    if (lblk < ZAEFS_NDIRECT) {
        if (!zi->di.direct[lblk] && alloc) {
            int64_t b = alloc_block(fs);
            if (b < 0) return -1;
            zi->di.direct[lblk] = (uint32_t)b;
            zi->di.nblocks++;
            zi->dirty = 1;
        }
        return zi->di.direct[lblk] ? zi->di.direct[lblk] : -1;
    }
    uint32_t *ptrs = kmalloc(BS);
    if (!ptrs) return -1;
    int64_t r = bmap_indirect(fs, zi, lblk - ZAEFS_NDIRECT, alloc, ptrs);
    kfree(ptrs);
    return r;
}

static int64_t bmap_indirect(struct zaefs *fs, struct zinode *zi, uint64_t lblk, int alloc, uint32_t *ptrs)
{

    uint32_t *slot;            /* where the pointer to the block we want lives */
    uint64_t idx_block;
    if (lblk < ZAEFS_PTRS_PER_BLOCK) {
        if (!zi->di.indirect) {
            if (!alloc) return -1;
            int64_t b = alloc_block(fs);
            if (b < 0) return -1;
            zi->di.indirect = (uint32_t)b;
            zi->di.nblocks++;
            zi->dirty = 1;
        }
        idx_block = zi->di.indirect;
        if (read_block(fs, idx_block, ptrs) != 0) return -1;
        slot = &ptrs[lblk];
    } else {
        lblk -= ZAEFS_PTRS_PER_BLOCK;
        if (lblk >= (uint64_t)ZAEFS_PTRS_PER_BLOCK * ZAEFS_PTRS_PER_BLOCK) return -1;
        if (!zi->di.dindirect) {
            if (!alloc) return -1;
            int64_t b = alloc_block(fs);
            if (b < 0) return -1;
            zi->di.dindirect = (uint32_t)b;
            zi->di.nblocks++;
            zi->dirty = 1;
        }
        if (read_block(fs, zi->di.dindirect, ptrs) != 0) return -1;
        uint64_t i1 = lblk / ZAEFS_PTRS_PER_BLOCK;
        if (!ptrs[i1]) {
            if (!alloc) return -1;
            int64_t b = alloc_block(fs);
            if (b < 0) return -1;
            ptrs[i1] = (uint32_t)b;
            zi->di.nblocks++;
            zi->dirty = 1;
            if (write_block(fs, zi->di.dindirect, ptrs) != 0) return -1;
        }
        idx_block = ptrs[i1];
        if (read_block(fs, idx_block, ptrs) != 0) return -1;
        slot = &ptrs[lblk % ZAEFS_PTRS_PER_BLOCK];
    }
    if (!*slot) {
        if (!alloc) return -1;
        int64_t b = alloc_block(fs);
        if (b < 0) return -1;
        *slot = (uint32_t)b;
        zi->di.nblocks++;
        zi->dirty = 1;
        if (write_block(fs, idx_block, ptrs) != 0) return -1;
    }
    return *slot;
}

/* Free every block of an inode (data and index). */
static void free_all_blocks(struct zaefs *fs, struct zinode *zi)
{
    uint32_t ptrs[ZAEFS_PTRS_PER_BLOCK], ptrs2[ZAEFS_PTRS_PER_BLOCK];
    for (int i = 0; i < ZAEFS_NDIRECT; i++)
        if (zi->di.direct[i]) free_block(fs, zi->di.direct[i]);
    if (zi->di.indirect && read_block(fs, zi->di.indirect, ptrs) == 0) {
        for (int i = 0; i < ZAEFS_PTRS_PER_BLOCK; i++)
            if (ptrs[i]) free_block(fs, ptrs[i]);
        free_block(fs, zi->di.indirect);
    }
    if (zi->di.dindirect && read_block(fs, zi->di.dindirect, ptrs) == 0) {
        for (int i = 0; i < ZAEFS_PTRS_PER_BLOCK; i++) {
            if (!ptrs[i]) continue;
            if (read_block(fs, ptrs[i], ptrs2) == 0)
                for (int j = 0; j < ZAEFS_PTRS_PER_BLOCK; j++)
                    if (ptrs2[j]) free_block(fs, ptrs2[j]);
            free_block(fs, ptrs[i]);
        }
        free_block(fs, zi->di.dindirect);
    }
    memset(zi->di.direct, 0, sizeof(zi->di.direct));
    zi->di.indirect = zi->di.dindirect = 0;
    zi->di.nblocks = 0;
    zi->di.size = 0;
    zi->dirty = 1;
}

/* ---- file data ---------------------------------------------------------------------- */

static long zaefs_read(struct vnode *n, uint64_t pos, void *buf, size_t len)
{
    struct zaefs *fs = fs_of(n);
    struct zinode *zi = zi_of(n);
    if (pos >= zi->di.size) return 0;
    if (pos + len > zi->di.size) len = (size_t)(zi->di.size - pos);

    uint8_t blk[BS];
    size_t done = 0;
    while (done < len) {
        uint64_t lblk = (pos + done) / BS, off = (pos + done) % BS;
        size_t n_ = BS - off;
        if (n_ > len - done) n_ = len - done;
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0)
            memset((uint8_t *)buf + done, 0, n_);          /* hole */
        else {
            if (read_block(fs, (uint64_t)pb, blk) != 0) break;
            memcpy((uint8_t *)buf + done, blk + off, n_);
        }
        done += n_;
    }
    return (long)done;
}

static long zaefs_write(struct vnode *n, uint64_t pos, const void *buf, size_t len)
{
    struct zaefs *fs = fs_of(n);
    struct zinode *zi = zi_of(n);
    uint8_t blk[BS];
    size_t done = 0;
    while (done < len) {
        uint64_t lblk = (pos + done) / BS, off = (pos + done) % BS;
        size_t n_ = BS - off;
        if (n_ > len - done) n_ = len - done;
        int64_t pb = bmap(fs, zi, lblk, 1);
        if (pb < 0) break;
        if (n_ != BS) {
            if (read_block(fs, (uint64_t)pb, blk) != 0) break;
        }
        memcpy(blk + off, (const uint8_t *)buf + done, n_);
        if (write_block(fs, (uint64_t)pb, blk) != 0) break;
        done += n_;
    }
    if (pos + done > zi->di.size) {
        zi->di.size = pos + done;
        n->size = zi->di.size;
    }
    zi->di.mtime = timer_ms() / 1000;
    zi->dirty = 1;
    zi_flush(n);
    return done ? (long)done : (len ? -1 : 0);
}

static int zaefs_truncate(struct vnode *n, uint64_t size)
{
    struct zinode *zi = zi_of(n);
    if (size != 0)
        return -1;              /* only truncate-to-zero for now */
    free_all_blocks(fs_of(n), zi);
    n->size = 0;
    return zi_flush(n);
}

/* ---- directories ------------------------------------------------------------------------ */

/* Iterate directory records: cb returns nonzero to stop; returns that value. */
struct dir_iter { uint8_t blk[BS]; uint64_t lblk; int64_t pb; uint32_t off; };

static struct vnode *make_vnode(struct vnode *dir, const char *name, size_t len, uint32_t ino)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = kzalloc(sizeof(*zi));
    if (!zi) return NULL;
    zi->ino = ino;
    if (inode_read(fs, ino, &zi->di) != 0) { kfree(zi); return NULL; }
    struct vnode *n = vnode_alloc(name, len, zi->di.type == ZAEFS_TYPE_DIR ? VNODE_DIR : VNODE_FILE, dir);
    if (!n) { kfree(zi); return NULL; }
    n->ino = ino;
    n->size = zi->di.size;
    n->priv = zi;
    return n;
}

static struct vnode *zaefs_lookup(struct vnode *dir, const char *name, size_t len)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(dir);
    uint8_t blk[BS];
    for (uint64_t lblk = 0; lblk * BS < zi->di.size; lblk++) {
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0 || read_block(fs, (uint64_t)pb, blk) != 0) continue;
        for (uint32_t off = 0; off + 8 <= BS;) {
            struct zaefs_dirent *e = (void *)(blk + off);
            if (e->rec_len < 8) break;
            if (e->ino && e->name_len == len && memcmp(e->name, name, len) == 0)
                return make_vnode(dir, name, len, e->ino);
            off += e->rec_len;
        }
    }
    return NULL;
}

static int dir_add(struct vnode *dir, const char *name, size_t len, uint32_t ino, uint8_t type)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(dir);
    uint32_t need = ZAEFS_DIRENT_SIZE(len);
    uint8_t blk[BS];

    for (uint64_t lblk = 0; lblk * BS < zi->di.size; lblk++) {
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0 || read_block(fs, (uint64_t)pb, blk) != 0) continue;
        for (uint32_t off = 0; off + 8 <= BS;) {
            struct zaefs_dirent *e = (void *)(blk + off);
            if (e->rec_len < 8) break;
            uint32_t used = e->ino ? ZAEFS_DIRENT_SIZE(e->name_len) : 0;
            if (e->rec_len - used >= need) {
                struct zaefs_dirent *ne = (void *)(blk + off + used);
                uint16_t total = e->rec_len;
                if (used) {
                    e->rec_len = (uint16_t)used;
                    ne->rec_len = (uint16_t)(total - used);
                }
                ne->ino = ino;
                ne->name_len = (uint8_t)len;
                ne->type = type;
                memcpy(ne->name, name, len);
                return write_block(fs, (uint64_t)pb, blk);
            }
            off += e->rec_len;
        }
    }
    /* No room: append a block holding just this record. */
    uint64_t lblk = zi->di.size / BS;
    int64_t pb = bmap(fs, zi, lblk, 1);
    if (pb < 0) return -1;
    memset(blk, 0, BS);
    struct zaefs_dirent *ne = (void *)blk;
    ne->ino = ino;
    ne->rec_len = BS;
    ne->name_len = (uint8_t)len;
    ne->type = type;
    memcpy(ne->name, name, len);
    if (write_block(fs, (uint64_t)pb, blk) != 0) return -1;
    zi->di.size = (lblk + 1) * BS;
    dir->size = zi->di.size;
    zi->dirty = 1;
    return zi_flush(dir);
}

static int dir_remove(struct vnode *dir, uint32_t ino)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(dir);
    uint8_t blk[BS];
    for (uint64_t lblk = 0; lblk * BS < zi->di.size; lblk++) {
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0 || read_block(fs, (uint64_t)pb, blk) != 0) continue;
        for (uint32_t off = 0; off + 8 <= BS;) {
            struct zaefs_dirent *e = (void *)(blk + off);
            if (e->rec_len < 8) break;
            if (e->ino == ino) {
                e->ino = 0;
                return write_block(fs, (uint64_t)pb, blk);
            }
            off += e->rec_len;
        }
    }
    return -1;
}

static int dir_is_empty(struct vnode *dir)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(dir);
    uint8_t blk[BS];
    for (uint64_t lblk = 0; lblk * BS < zi->di.size; lblk++) {
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0 || read_block(fs, (uint64_t)pb, blk) != 0) continue;
        for (uint32_t off = 0; off + 8 <= BS;) {
            struct zaefs_dirent *e = (void *)(blk + off);
            if (e->rec_len < 8) break;
            if (e->ino) return 0;
            off += e->rec_len;
        }
    }
    return 1;
}

static struct vnode *zaefs_create(struct vnode *dir, const char *name, size_t len, enum vnode_type type)
{
    struct zaefs *fs = fs_of(dir);
    if (type == VNODE_DEV || len > ZAEFS_NAME_MAX) return NULL;
    int64_t ino = alloc_inode(fs);
    if (ino < 0) return NULL;

    struct zaefs_inode di;
    memset(&di, 0, sizeof(di));
    di.type = type == VNODE_DIR ? ZAEFS_TYPE_DIR : ZAEFS_TYPE_FILE;
    di.links = 1;
    di.mode = type == VNODE_DIR ? 0755 : 0644;
    di.ctime = di.mtime = timer_ms() / 1000;
    if (inode_write(fs, (uint32_t)ino, &di) != 0 ||
        dir_add(dir, name, len, (uint32_t)ino, (uint8_t)di.type) != 0) {
        free_inode(fs, (uint64_t)ino);
        return NULL;
    }
    return make_vnode(dir, name, len, (uint32_t)ino);
}

static int zaefs_unlink(struct vnode *dir, struct vnode *n)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(n);
    if (n->type == VNODE_DIR && !dir_is_empty(n)) return -1;
    if (dir_remove(dir, zi->ino) != 0) return -1;
    free_all_blocks(fs, zi);
    free_inode(fs, zi->ino);
    kfree(zi);
    n->priv = NULL;
    return 0;
}

static int zaefs_readdir(struct vnode *dir, size_t index, struct dirent *out)
{
    struct zaefs *fs = fs_of(dir);
    struct zinode *zi = zi_of(dir);
    uint8_t blk[BS];
    for (uint64_t lblk = 0; lblk * BS < zi->di.size; lblk++) {
        int64_t pb = bmap(fs, zi, lblk, 0);
        if (pb < 0 || read_block(fs, (uint64_t)pb, blk) != 0) continue;
        for (uint32_t off = 0; off + 8 <= BS;) {
            struct zaefs_dirent *e = (void *)(blk + off);
            if (e->rec_len < 8) break;
            if (e->ino) {
                if (index == 0) {
                    size_t l = e->name_len < VFS_NAME_MAX - 1 ? e->name_len : VFS_NAME_MAX - 1;
                    memset(out->name, 0, VFS_NAME_MAX);
                    memcpy(out->name, e->name, l);
                    out->type = e->type == ZAEFS_TYPE_DIR ? VNODE_DIR : VNODE_FILE;
                    out->ino = e->ino;
                    struct zaefs_inode di;
                    out->size = inode_read(fs, e->ino, &di) == 0 ? di.size : 0;
                    return 1;
                }
                index--;
            }
            off += e->rec_len;
        }
    }
    return 0;
}

static const struct vnode_ops zaefs_ops = {
    .lookup   = zaefs_lookup,
    .create   = zaefs_create,
    .unlink   = zaefs_unlink,
    .read     = zaefs_read,
    .write    = zaefs_write,
    .truncate = zaefs_truncate,
    .readdir  = zaefs_readdir,
    .sync     = zi_flush,
};

/* ---- mount ----------------------------------------------------------------------------- */

static struct vnode *zaefs_mount(struct vfs_mount *mnt, struct vnode *devnode, const char *opts)
{
    (void)opts;
    struct blkdev *dev = blkdev_from_vnode(devnode);
    if (!dev || dev->sector_size > BS || BS % dev->sector_size) {
        kprintf("zaefs: not a usable block device\n");
        return NULL;
    }
    struct zaefs *fs = kzalloc(sizeof(*fs));
    if (!fs) return NULL;
    fs->cache = kzalloc(sizeof(struct cache_slot) * CACHE_SLOTS);
    if (!fs->cache) { kfree(fs); return NULL; }
    fs->dev = dev;
    fs->sectors_per_block = BS / dev->sector_size;

    if (read_block(fs, 0, &fs->sb) != 0 || fs->sb.magic != ZAEFS_MAGIC ||
        fs->sb.version != ZAEFS_VERSION || fs->sb.block_size != BS ||
        fs->sb.total_blocks * fs->sectors_per_block > dev->sectors) {
        kprintf("zaefs: %s: no valid zaefs superblock\n", dev->name);
        kfree(fs->cache);
        kfree(fs);
        return NULL;
    }
    fs->block_hint = fs->sb.data_start;
    fs->inode_hint = 1;
    mnt->priv = fs;

    struct vnode *root = vnode_alloc("", 0, VNODE_DIR, NULL);
    struct zinode *zi = kzalloc(sizeof(*zi));
    if (!root || !zi) { kfree(root); kfree(zi); kfree(fs->cache); kfree(fs); return NULL; }
    zi->ino = (uint32_t)fs->sb.root_ino;
    if (inode_read(fs, zi->ino, &zi->di) != 0) { kfree(root); kfree(zi); kfree(fs->cache); kfree(fs); return NULL; }
    root->mnt = mnt;
    root->ops = &zaefs_ops;
    root->ino = zi->ino;
    root->size = zi->di.size;
    root->priv = zi;

    kprintf("zaefs: mounted %s \"%s\": %lu blocks (%lu free), %lu inodes (%lu free)\n",
            dev->name, fs->sb.label, fs->sb.total_blocks, fs->sb.free_blocks,
            fs->sb.inode_count, fs->sb.free_inodes);
    return root;
}

static void drop_tree(struct vnode *n)
{
    for (struct vnode *c = n->children, *next; c; c = next) {
        next = c->sibling;
        drop_tree(c);
    }
    zi_flush(n);
    kfree(n->priv);
    vnode_free(n);
}

static int zaefs_unmount(struct vfs_mount *mnt)
{
    struct zaefs *fs = mnt->priv;
    drop_tree(mnt->root);
    kfree(fs->cache);
    kfree(fs);
    return 0;
}

struct fs_type zaefs_type = { .name = "zaefs", .mount = zaefs_mount, .unmount = zaefs_unmount };
