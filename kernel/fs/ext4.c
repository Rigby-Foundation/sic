/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ext4 (and ext2/ext3), read/write (CONFIG_EXT4): what a phone's recovery
 * formats Data and SD cards with. Files and directories are read through
 * extents or the old block maps; what is written uses extents.
 *
 * Writes keep everything Linux and e2fsck check: block and inode bitmaps
 * (groups mke2fs left uninitialised are set up on first use), group
 * descriptors, the superblock's counts, and with metadata_csum the CRC32C
 * of each (and of inodes, extent blocks, directory blocks, htree nodes).
 * New names go into hashed (htree) directories where the hash says, a
 * leaf that is full split in two. The journal is not used: a mounted
 * filesystem is marked not clean until unmount or sync, so a crash shows
 * up in the next e2fsck. Encryption, case folding, inline data, a journal
 * that needs replaying or an unknown feature mean a read-only mount. */
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"

/* features */
#define COMPAT_DIR_INDEX        0x0020
#define INCOMPAT_FILETYPE       0x0002
#define INCOMPAT_RECOVER        0x0004
#define INCOMPAT_JOURNAL_DEV    0x0008
#define INCOMPAT_META_BG        0x0010
#define INCOMPAT_EXTENTS        0x0040
#define INCOMPAT_64BIT          0x0080
#define INCOMPAT_MMP            0x0100
#define INCOMPAT_FLEX_BG        0x0200
#define INCOMPAT_EA_INODE       0x0400
#define INCOMPAT_DIRDATA        0x1000
#define INCOMPAT_CSUM_SEED      0x2000
#define INCOMPAT_LARGEDIR       0x4000
#define INCOMPAT_INLINE_DATA    0x8000
#define INCOMPAT_ENCRYPT        0x10000
#define INCOMPAT_CASEFOLD       0x20000
#define RO_COMPAT_GDT_CSUM      0x0010
#define RO_COMPAT_METADATA_CSUM 0x0400
#define RO_COMPAT_QUOTA         0x0100
#define INCOMPAT_READ_OK  (INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_META_BG | INCOMPAT_EXTENTS | INCOMPAT_64BIT | \
                           INCOMPAT_MMP | INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR)
#define INCOMPAT_WRITE_OK (INCOMPAT_FILETYPE | INCOMPAT_META_BG | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | \
                           INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR)

/* superblock offsets (Linux's struct ext4_super_block) */
#define SB_INODES_COUNT     0x00
#define SB_BLOCKS_LO        0x04
#define SB_FREE_BLOCKS_LO   0x0c
#define SB_FREE_INODES      0x10
#define SB_FIRST_DATA_BLOCK 0x14
#define SB_LOG_BLOCK_SIZE   0x18
#define SB_BLOCKS_PER_GROUP 0x20
#define SB_INODES_PER_GROUP 0x28
#define SB_MTIME            0x2c
#define SB_WTIME            0x30
#define SB_MNT_COUNT        0x34
#define SB_MAGIC            0x38
#define SB_STATE            0x3a
#define SB_REV_LEVEL        0x4c
#define SB_FIRST_INO        0x54
#define SB_INODE_SIZE       0x58
#define SB_FEATURE_COMPAT   0x5c
#define SB_FEATURE_INCOMPAT 0x60
#define SB_FEATURE_RO       0x64
#define SB_UUID             0x68
#define SB_VOLUME_NAME      0x78
#define SB_RESERVED_GDT     0xce
#define SB_HASH_SEED        0xec
#define SB_DEF_HASH_VERSION 0xfc
#define SB_DESC_SIZE        0xfe
#define SB_FIRST_META_BG    0x104
#define SB_BLOCKS_HI        0x150
#define SB_FREE_BLOCKS_HI   0x158
#define SB_WANT_EXTRA_ISIZE 0x15e
#define SB_FLAGS            0x160
#define SB_CHECKSUM_SEED    0x270
#define SB_CHECKSUM         0x3fc

/* group descriptor offsets */
#define GD_BLOCK_BITMAP     0x00
#define GD_INODE_BITMAP     0x04
#define GD_INODE_TABLE      0x08
#define GD_FREE_BLOCKS      0x0c
#define GD_FREE_INODES      0x0e
#define GD_USED_DIRS        0x10
#define GD_FLAGS            0x12
#define GD_BBITMAP_CSUM     0x18
#define GD_IBITMAP_CSUM     0x1a
#define GD_ITABLE_UNUSED    0x1c
#define GD_CHECKSUM         0x1e
#define GD_BLOCK_BITMAP_HI  0x20
#define GD_INODE_BITMAP_HI  0x24
#define GD_INODE_TABLE_HI   0x28
#define GD_FREE_BLOCKS_HI   0x2c
#define GD_FREE_INODES_HI   0x2e
#define GD_USED_DIRS_HI     0x30
#define GD_ITABLE_UNUSED_HI 0x32
#define GD_BBITMAP_CSUM_HI  0x38
#define GD_IBITMAP_CSUM_HI  0x3a
#define BG_INODE_UNINIT     0x1
#define BG_BLOCK_UNINIT     0x2

/* inode offsets */
#define I_MODE              0x00
#define I_SIZE_LO           0x04
#define I_ATIME             0x08
#define I_CTIME             0x0c
#define I_MTIME             0x10
#define I_DTIME             0x14
#define I_LINKS             0x1a
#define I_BLOCKS_LO         0x1c
#define I_FLAGS             0x20
#define I_BLOCK             0x28
#define I_GENERATION        0x64
#define I_FILE_ACL          0x68
#define I_SIZE_HI           0x6c
#define I_BLOCKS_HI         0x74
#define I_CHECKSUM_LO       0x7c
#define I_EXTRA_ISIZE       0x80
#define I_CHECKSUM_HI       0x82
#define I_CRTIME            0x90
#define FL_INDEX            0x00001000
#define FL_HUGE_FILE        0x00040000
#define FL_EXTENTS          0x00080000
#define FL_INLINE_DATA      0x10000000
#define EXT_S_IFMT              0xf000
#define EXT_S_IFDIR             0x4000
#define EXT_S_IFREG             0x8000

#define EXT_MAGIC           0xf30a
#define EXT_INIT_MAX_LEN    32768
#define ROOT_INO            2

struct ext4 {
    struct blkdev *dev;
    uint8_t sb[1024];
    uint32_t bs, groups, ipg, bpg, inode_size, desc_size, first_data, gdt_blocks, reserved_gdt, first_meta_bg;
    uint64_t blocks;
    uint8_t *gdt;                       /* every group descriptor */
    uint8_t **bbm, **ibm;               /* bitmaps, read when first needed */
    uint8_t *bbm_dirty, *ibm_dirty, *gd_dirty;
    int rw, csum, gdt_csum, bit64, meta_bg, sparse, ro_reason_logged;
    uint32_t seed;                      /* the metadata checksum seed */
    uint32_t hash_seed[4];
    int hash_version;
    uint8_t *blk, *blk2, *blk3;         /* scratch blocks */
    uint8_t *ibuf;                      /* an inode */
    volatile int busy;
    uint32_t next_gen;
    int sb_dirty;
};

struct enode { uint32_t ino; };

#define EFS(n)  ((struct ext4 *)(n)->mnt->priv)
#define INO(n)  (((struct enode *)(n)->priv)->ino)

/* ---- little-endian fields, byte by byte (no unaligned loads) ------------------------ */

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- CRC32C (Castagnoli, raw: no final inversion, as Linux's crc32c()) and CRC16 ---- */

static uint32_t crc32c_table[256];
static uint16_t crc16_table[256];

static void crc_init(void)
{
    if (crc32c_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        uint16_t d = (uint16_t)i;
        for (int k = 0; k < 8; k++) {
            c = c & 1 ? (c >> 1) ^ 0x82f63b78 : c >> 1;
            d = d & 1 ? (uint16_t)((d >> 1) ^ 0xa001) : (uint16_t)(d >> 1);
        }
        crc32c_table[i] = c;
        crc16_table[i] = d;
    }
}

static uint32_t crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--) crc = crc32c_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc;
}

static uint16_t crc16(uint16_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--) crc = (uint16_t)(crc16_table[(crc ^ *p++) & 0xff] ^ (crc >> 8));
    return crc;
}

/* ---- blocks ------------------------------------------------------------------------- */

static int bread(struct ext4 *fs, uint64_t b, void *buf)
{
    if (!b || b >= fs->blocks) return -1;
    return blkdev_read_bytes(fs->dev, b * fs->bs, buf, fs->bs) == (long)fs->bs ? 0 : -1;
}

static int bwrite(struct ext4 *fs, uint64_t b, const void *buf)
{
    if (!fs->rw || !b || b >= fs->blocks) return -1;
    return blkdev_write_bytes(fs->dev, b * fs->bs, buf, fs->bs) == (long)fs->bs ? 0 : -1;
}

static void lock(struct ext4 *fs) { while (__atomic_exchange_n(&fs->busy, 1, __ATOMIC_ACQUIRE)) task_sleep_ms(1); }
static void unlock(struct ext4 *fs) { __atomic_store_n(&fs->busy, 0, __ATOMIC_RELEASE); }

static uint32_t now(void) { return (uint32_t)(timer_boot_epoch() + timer_ms() / 1000); }

/* ---- group descriptors and bitmaps -------------------------------------------------------- */

static uint8_t *gd(struct ext4 *fs, uint32_t g) { return fs->gdt + (size_t)g * fs->desc_size; }

static uint64_t gd64(struct ext4 *fs, uint32_t g, uint32_t lo, uint32_t hi)
{
    uint8_t *d = gd(fs, g);
    return r32(d + lo) | (fs->desc_size >= 64 ? (uint64_t)r32(d + hi) << 32 : 0);
}

static uint32_t gd32(struct ext4 *fs, uint32_t g, uint32_t lo, uint32_t hi)
{
    uint8_t *d = gd(fs, g);
    return r16(d + lo) | (fs->desc_size >= 64 ? (uint32_t)r16(d + hi) << 16 : 0);
}

static void gd32_set(struct ext4 *fs, uint32_t g, uint32_t lo, uint32_t hi, uint32_t v)
{
    uint8_t *d = gd(fs, g);
    w16(d + lo, v);
    if (fs->desc_size >= 64) w16(d + hi, v >> 16);
    fs->gd_dirty[g] = 1;
}

static void gd_csum(struct ext4 *fs, uint32_t g)
{
    uint8_t *d = gd(fs, g), le[4];
    w32(le, g);
    if (fs->csum) {
        uint32_t c = crc32c(fs->seed, le, 4);
        c = crc32c(c, d, GD_CHECKSUM);
        c = crc32c(c, "\0\0", 2);
        if (fs->desc_size > GD_CHECKSUM + 2) c = crc32c(c, d + GD_CHECKSUM + 2, fs->desc_size - GD_CHECKSUM - 2);
        w16(d + GD_CHECKSUM, c & 0xffff);
    } else if (fs->gdt_csum) {
        uint16_t c = crc16(0xffff, fs->sb + SB_UUID, 16);
        c = crc16(c, le, 4);
        c = crc16(c, d, GD_CHECKSUM);
        if (fs->desc_size > GD_CHECKSUM + 2) c = crc16(c, d + GD_CHECKSUM + 2, fs->desc_size - GD_CHECKSUM - 2);
        w16(d + GD_CHECKSUM, c);
    }
}

/* Groups 0, 1 and powers of 3, 5 and 7 keep a superblock copy (sparse_super). */
static int has_super(struct ext4 *fs, uint32_t g)
{
    if (!fs->sparse || g <= 1) return 1;
    for (uint32_t b = 3; b <= 7; b += 2) {
        uint32_t p = b;
        while (p < g) p *= b;
        if (p == g) return 1;
    }
    return 0;
}

static uint64_t group_first(struct ext4 *fs, uint32_t g) { return fs->first_data + (uint64_t)g * fs->bpg; }
static uint32_t group_blocks(struct ext4 *fs, uint32_t g)
{
    uint64_t end = group_first(fs, g) + fs->bpg;
    return (uint32_t)((end > fs->blocks ? fs->blocks : end) - group_first(fs, g));
}

/* What a group mke2fs left as BLOCK_UNINIT holds: its superblock copy and
 * descriptors, and any group's bitmaps and inode table that sit in it. */
static void init_block_bitmap(struct ext4 *fs, uint32_t g, uint8_t *bm)
{
    memset(bm, 0, fs->bs);
    uint64_t first = group_first(fs, g);
    uint32_t n = 0;
    if (has_super(fs, g)) {
        n = 1;
        if (!fs->meta_bg) n += fs->gdt_blocks + fs->reserved_gdt;
        else {
            uint32_t per = fs->bs / fs->desc_size, mg = g / per;
            if (mg >= fs->first_meta_bg) n = (g % per == 0 || g % per == 1 || g % per == per - 1) ? 2 : 1;
            else n += fs->gdt_blocks + fs->reserved_gdt;
        }
    }
    for (uint32_t i = 0; i < n; i++) bm[i / 8] |= (uint8_t)(1 << (i % 8));
    uint32_t itable_blocks = (fs->ipg * fs->inode_size + fs->bs - 1) / fs->bs;
    for (uint32_t o = 0; o < fs->groups; o++) {
        uint64_t marks[3] = { gd64(fs, o, GD_BLOCK_BITMAP, GD_BLOCK_BITMAP_HI), gd64(fs, o, GD_INODE_BITMAP, GD_INODE_BITMAP_HI), 0 };
        uint64_t it = gd64(fs, o, GD_INODE_TABLE, GD_INODE_TABLE_HI);
        for (int k = 0; k < 2; k++)
            if (marks[k] >= first && marks[k] < first + fs->bpg) { uint32_t b = (uint32_t)(marks[k] - first); bm[b / 8] |= (uint8_t)(1 << (b % 8)); }
        for (uint32_t k = 0; k < itable_blocks; k++)
            if (it + k >= first && it + k < first + fs->bpg) { uint32_t b = (uint32_t)(it + k - first); bm[b / 8] |= (uint8_t)(1 << (b % 8)); }
    }
    for (uint32_t b = group_blocks(fs, g); b < fs->bs * 8; b++) bm[b / 8] |= (uint8_t)(1 << (b % 8));   /* past the end */
}

static uint8_t *block_bitmap(struct ext4 *fs, uint32_t g)
{
    if (fs->bbm[g]) return fs->bbm[g];
    uint8_t *bm = kmalloc(fs->bs);
    if (!bm) return NULL;
    if ((fs->csum || fs->gdt_csum) && (r16(gd(fs, g) + GD_FLAGS) & BG_BLOCK_UNINIT)) init_block_bitmap(fs, g, bm);
    else if (bread(fs, gd64(fs, g, GD_BLOCK_BITMAP, GD_BLOCK_BITMAP_HI), bm)) { kfree(bm); return NULL; }
    fs->bbm[g] = bm;
    return bm;
}

static uint8_t *inode_bitmap(struct ext4 *fs, uint32_t g)
{
    if (fs->ibm[g]) return fs->ibm[g];
    uint8_t *bm = kmalloc(fs->bs);
    if (!bm) return NULL;
    if ((fs->csum || fs->gdt_csum) && (r16(gd(fs, g) + GD_FLAGS) & BG_INODE_UNINIT)) {
        memset(bm, 0, fs->bs);
        for (uint32_t i = fs->ipg; i < fs->bs * 8; i++) bm[i / 8] |= (uint8_t)(1 << (i % 8));
    } else if (bread(fs, gd64(fs, g, GD_INODE_BITMAP, GD_INODE_BITMAP_HI), bm)) { kfree(bm); return NULL; }
    fs->ibm[g] = bm;
    return bm;
}

/* ---- inodes ------------------------------------------------------------------------- */

static int inode_loc(struct ext4 *fs, uint32_t ino, uint64_t *blk, uint32_t *off)
{
    if (!ino || ino > r32(fs->sb + SB_INODES_COUNT)) return -1;
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
    uint64_t byte = (uint64_t)idx * fs->inode_size;
    *blk = gd64(fs, g, GD_INODE_TABLE, GD_INODE_TABLE_HI) + byte / fs->bs;
    *off = (uint32_t)(byte % fs->bs);
    return 0;
}

static int iread(struct ext4 *fs, uint32_t ino, uint8_t *in)
{
    uint64_t b; uint32_t off;
    if (inode_loc(fs, ino, &b, &off) || bread(fs, b, fs->blk3)) return -1;
    memcpy(in, fs->blk3 + off, fs->inode_size);
    return 0;
}

static int has_extra(struct ext4 *fs, const uint8_t *in, uint32_t field_end)
{
    return fs->inode_size > 128 && 128 + r16(in + I_EXTRA_ISIZE) >= field_end;
}

/* The seed of an inode's own checksums (its blocks too): seed, number, generation. */
static uint32_t inode_seed(struct ext4 *fs, uint32_t ino, const uint8_t *in)
{
    uint8_t le[4];
    w32(le, ino);
    uint32_t c = crc32c(fs->seed, le, 4);
    return crc32c(c, in + I_GENERATION, 4);
}

static int iwrite(struct ext4 *fs, uint32_t ino, uint8_t *in)
{
    if (fs->csum) {
        int hi = has_extra(fs, in, I_CHECKSUM_HI + 2);
        w16(in + I_CHECKSUM_LO, 0);
        if (hi) w16(in + I_CHECKSUM_HI, 0);
        uint32_t c = crc32c(inode_seed(fs, ino, in), in, fs->inode_size);
        w16(in + I_CHECKSUM_LO, c & 0xffff);
        if (hi) w16(in + I_CHECKSUM_HI, c >> 16);
    }
    uint64_t b; uint32_t off;
    if (inode_loc(fs, ino, &b, &off) || bread(fs, b, fs->blk3)) return -1;
    memcpy(fs->blk3 + off, in, fs->inode_size);
    return bwrite(fs, b, fs->blk3);
}

static uint64_t isize(const uint8_t *in) { return r32(in + I_SIZE_LO) | (uint64_t)r32(in + I_SIZE_HI) << 32; }
static void isize_set(uint8_t *in, uint64_t s) { w32(in + I_SIZE_LO, (uint32_t)s); w32(in + I_SIZE_HI, (uint32_t)(s >> 32)); }

/* i_blocks counts 512-byte sectors (huge_file inodes aside, which we don't make). */
static void iblocks_add(struct ext4 *fs, uint8_t *in, int64_t blocks)
{
    uint64_t v = r32(in + I_BLOCKS_LO) | (uint64_t)r16(in + I_BLOCKS_HI) << 32;
    v += (uint64_t)(blocks * (int64_t)(fs->bs / 512));
    w32(in + I_BLOCKS_LO, (uint32_t)v);
    w16(in + I_BLOCKS_HI, (uint32_t)(v >> 32));
}

/* ---- allocation ---------------------------------------------------------------------- */

static void sb_free_blocks_add(struct ext4 *fs, int64_t d)
{
    uint64_t v = r32(fs->sb + SB_FREE_BLOCKS_LO) | (uint64_t)r32(fs->sb + SB_FREE_BLOCKS_HI) << 32;
    v += (uint64_t)d;
    w32(fs->sb + SB_FREE_BLOCKS_LO, (uint32_t)v);
    w32(fs->sb + SB_FREE_BLOCKS_HI, (uint32_t)(v >> 32));
    fs->sb_dirty = 1;
}

static void group_take_block_bitmap(struct ext4 *fs, uint32_t g)
{
    uint8_t *d = gd(fs, g);
    if (r16(d + GD_FLAGS) & BG_BLOCK_UNINIT) w16(d + GD_FLAGS, r16(d + GD_FLAGS) & ~BG_BLOCK_UNINIT);
    fs->bbm_dirty[g] = 1;
    fs->gd_dirty[g] = 1;
}

/* A free block, the first at or after goal (then from the start). */
static uint64_t balloc(struct ext4 *fs, uint64_t goal)
{
    if (goal < fs->first_data || goal >= fs->blocks) goal = fs->first_data;
    uint32_t g0 = (uint32_t)((goal - fs->first_data) / fs->bpg);
    for (uint32_t k = 0; k <= fs->groups; k++) {
        uint32_t g = (g0 + k) % fs->groups;
        if (!gd32(fs, g, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI)) continue;
        uint8_t *bm = block_bitmap(fs, g);
        if (!bm) continue;
        uint32_t start = k == 0 ? (uint32_t)((goal - fs->first_data) % fs->bpg) : 0, n = group_blocks(fs, g);
        for (uint32_t pass = 0; pass < 2; pass++)
            for (uint32_t b = pass ? 0 : start; b < (pass ? start : n); b++) {
                if (bm[b / 8] == 0xff && b % 8 == 0) { b += 7; continue; }
                if (bm[b / 8] & (1 << (b % 8))) continue;
                bm[b / 8] |= (uint8_t)(1 << (b % 8));
                group_take_block_bitmap(fs, g);
                gd32_set(fs, g, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI, gd32(fs, g, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI) - 1);
                sb_free_blocks_add(fs, -1);
                return group_first(fs, g) + b;
            }
    }
    return 0;
}

static void bfree(struct ext4 *fs, uint64_t blk)
{
    if (blk < fs->first_data || blk >= fs->blocks) return;
    uint32_t g = (uint32_t)((blk - fs->first_data) / fs->bpg), b = (uint32_t)((blk - fs->first_data) % fs->bpg);
    uint8_t *bm = block_bitmap(fs, g);
    if (!bm || !(bm[b / 8] & (1 << (b % 8)))) return;
    bm[b / 8] &= (uint8_t)~(1 << (b % 8));
    group_take_block_bitmap(fs, g);
    gd32_set(fs, g, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI, gd32(fs, g, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI) + 1);
    sb_free_blocks_add(fs, 1);
}

/* A free inode, near its parent's group; dirs counted in their group. */
static uint32_t ialloc(struct ext4 *fs, uint32_t parent, int dir)
{
    uint32_t g0 = (parent - 1) / fs->ipg, first = r32(fs->sb + SB_FIRST_INO);
    for (uint32_t k = 0; k < fs->groups; k++) {
        uint32_t g = (g0 + k) % fs->groups;
        if (!gd32(fs, g, GD_FREE_INODES, GD_FREE_INODES_HI)) continue;
        uint8_t *bm = inode_bitmap(fs, g);
        if (!bm) continue;
        for (uint32_t i = 0; i < fs->ipg; i++) {
            uint32_t ino = g * fs->ipg + i + 1;
            if (ino < first || (bm[i / 8] & (1 << (i % 8)))) continue;
            bm[i / 8] |= (uint8_t)(1 << (i % 8));
            fs->ibm_dirty[g] = 1;
            uint8_t *d = gd(fs, g);
            if (r16(d + GD_FLAGS) & BG_INODE_UNINIT) w16(d + GD_FLAGS, r16(d + GD_FLAGS) & ~BG_INODE_UNINIT);
            gd32_set(fs, g, GD_FREE_INODES, GD_FREE_INODES_HI, gd32(fs, g, GD_FREE_INODES, GD_FREE_INODES_HI) - 1);
            if (dir) gd32_set(fs, g, GD_USED_DIRS, GD_USED_DIRS_HI, gd32(fs, g, GD_USED_DIRS, GD_USED_DIRS_HI) + 1);
            if (fs->csum || fs->gdt_csum) {                     /* the part of the table in use */
                uint32_t unused = gd32(fs, g, GD_ITABLE_UNUSED, GD_ITABLE_UNUSED_HI);
                if (i + 1 > fs->ipg - unused) gd32_set(fs, g, GD_ITABLE_UNUSED, GD_ITABLE_UNUSED_HI, fs->ipg - (i + 1));
            }
            w32(fs->sb + SB_FREE_INODES, r32(fs->sb + SB_FREE_INODES) - 1);
            fs->sb_dirty = 1;
            return ino;
        }
    }
    return 0;
}

static void ifree(struct ext4 *fs, uint32_t ino, int dir)
{
    uint32_t g = (ino - 1) / fs->ipg, i = (ino - 1) % fs->ipg;
    uint8_t *bm = inode_bitmap(fs, g);
    if (!bm || !(bm[i / 8] & (1 << (i % 8)))) return;
    bm[i / 8] &= (uint8_t)~(1 << (i % 8));
    fs->ibm_dirty[g] = 1;
    gd32_set(fs, g, GD_FREE_INODES, GD_FREE_INODES_HI, gd32(fs, g, GD_FREE_INODES, GD_FREE_INODES_HI) + 1);
    if (dir) gd32_set(fs, g, GD_USED_DIRS, GD_USED_DIRS_HI, gd32(fs, g, GD_USED_DIRS, GD_USED_DIRS_HI) - 1);
    w32(fs->sb + SB_FREE_INODES, r32(fs->sb + SB_FREE_INODES) + 1);
    fs->sb_dirty = 1;
}

/* ---- extents ------------------------------------------------------------------------- */

/* An extent node: header (magic, entries, max, depth), then 12-byte
 * entries: leaf (block, len, start hi16, start lo32), index (block, leaf lo32, leaf hi16). */
static uint64_t ext_start(const uint8_t *e) { return r32(e + 8) | (uint64_t)r16(e + 6) << 32; }
static uint64_t idx_leaf(const uint8_t *e) { return r32(e + 4) | (uint64_t)r16(e + 8) << 32; }

static void ext_tail_csum(struct ext4 *fs, uint32_t ino, const uint8_t *in, uint8_t *node)
{
    if (!fs->csum) return;
    uint32_t off = 12 + 12 * r16(node + 4);
    w32(node + off, crc32c(inode_seed(fs, ino, in), node, off));
}

/* The physical block of a logical one; 0 for a hole (or unwritten extent). */
static uint64_t bmap(struct ext4 *fs, const uint8_t *in, uint64_t lblk)
{
    if (r32(in + I_FLAGS) & FL_EXTENTS) {
        uint8_t node[60];
        memcpy(node, in + I_BLOCK, 60);
        const uint8_t *h = node;
        uint8_t *buf = NULL;
        for (int level = 0; level < 6; level++) {
            if (r16(h) != EXT_MAGIC) break;
            uint16_t n = r16(h + 2), depth = r16(h + 6);
            const uint8_t *e = h + 12, *pick = NULL;
            if (!depth) {
                for (uint16_t i = 0; i < n; i++, e += 12) {
                    uint32_t b = r32(e), len = r16(e + 4);
                    if (len > EXT_INIT_MAX_LEN) { len -= EXT_INIT_MAX_LEN; if (lblk >= b && lblk < b + len) { kfree(buf); return 0; } }
                    if (lblk >= b && lblk < b + len) { uint64_t p = ext_start(e) + (lblk - b); kfree(buf); return p; }
                }
                break;
            }
            for (uint16_t i = 0; i < n; i++, e += 12) if (r32(e) <= lblk) pick = e;
            if (!pick) break;
            if (!buf && !(buf = kmalloc(fs->bs))) break;
            if (bread(fs, idx_leaf(pick), buf)) break;
            h = buf;
        }
        kfree(buf);
        return 0;
    }
    /* the old way: 12 direct, then single, double, triple indirect */
    uint32_t per = fs->bs / 4;
    if (lblk < 12) return r32(in + I_BLOCK + 4 * lblk);
    lblk -= 12;
    uint64_t span = 1, b = 0;
    int level;
    for (level = 1; level <= 3; level++) {
        span *= per;
        if (lblk < span) { b = r32(in + I_BLOCK + 4 * (11 + level)); break; }
        lblk -= span;
    }
    if (level > 3) return 0;
    uint8_t *buf = kmalloc(fs->bs);
    if (!buf) return 0;
    for (; level > 0 && b; level--) {
        span /= per;
        if (bread(fs, b, buf)) { b = 0; break; }
        b = r32(buf + 4 * (lblk / span));
        lblk %= span;
    }
    kfree(buf);
    return b;
}

/* Map lblk to the new block pblk in an extents inode: grow the last
 * extent when it lines up, else a new one; the root (4 in the inode) is
 * pushed into a leaf block when full, a full leaf gets a sibling.
 * Appending is what this is for; a write into a hole of a full leaf fails. */
static int ext_insert(struct ext4 *fs, uint32_t ino, uint8_t *in, uint64_t lblk, uint64_t pblk)
{
    uint8_t *root = in + I_BLOCK;
    uint16_t depth = r16(root + 6);
    if (depth > 1) return -1;
    uint8_t *leaf = root, *buf = NULL;
    uint64_t leaf_blk = 0;
    if (depth == 1) {
        uint16_t n = r16(root + 2);
        uint8_t *pick = root + 12;
        for (uint16_t i = 0; i < n; i++) if (r32(root + 12 + 12 * i) <= lblk) pick = root + 12 + 12 * i;
        leaf_blk = idx_leaf(pick);
        if (!(buf = kmalloc(fs->bs)) || bread(fs, leaf_blk, buf)) { kfree(buf); return -1; }
        leaf = buf;
    }
    uint16_t n = r16(leaf + 2), max = r16(leaf + 4);
    /* where it goes: after the last extent that starts before it */
    uint16_t at = 0;
    while (at < n && r32(leaf + 12 + 12 * at) < lblk) at++;
    if (at > 0) {
        uint8_t *prev = leaf + 12 + 12 * (at - 1);
        uint32_t len = r16(prev + 4);
        if (len < EXT_INIT_MAX_LEN && r32(prev) + len == lblk && ext_start(prev) + len == pblk) {
            w16(prev + 4, len + 1);
            goto done;
        }
    }
    if (n < max) {
        memmove(leaf + 12 + 12 * (at + 1), leaf + 12 + 12 * at, 12u * (n - at));
        uint8_t *e = leaf + 12 + 12 * at;
        w32(e, (uint32_t)lblk); w16(e + 4, 1); w16(e + 6, (uint32_t)(pblk >> 32)); w32(e + 8, (uint32_t)pblk);
        w16(leaf + 2, n + 1);
        goto done;
    }
    if (depth == 0) {                                   /* the inode's 4 are full: into a leaf block */
        uint64_t nb = balloc(fs, pblk);
        if (!nb) return -1;
        uint8_t *nl = kzalloc(fs->bs);
        if (!nl) { bfree(fs, nb); return -1; }
        w16(nl, EXT_MAGIC); w16(nl + 2, n); w16(nl + 4, (fs->bs - 12 - (fs->csum ? 4 : 0)) / 12); w16(nl + 6, 0);
        memcpy(nl + 12, root + 12, 12u * n);
        w16(root + 2, 1); w16(root + 6, 1);
        uint8_t *ix = root + 12;
        w32(ix, r32(nl + 12)); w32(ix + 4, (uint32_t)nb); w16(ix + 8, (uint32_t)(nb >> 32)); w16(ix + 10, 0);
        iblocks_add(fs, in, 1);
        ext_tail_csum(fs, ino, in, nl);
        int rc = bwrite(fs, nb, nl);
        kfree(nl);
        return rc ? -1 : ext_insert(fs, ino, in, lblk, pblk);
    }
    /* a full leaf: appending past its end starts a sibling */
    if (at != n || r16(root + 2) >= r16(root + 4)) { kfree(buf); return -1; }
    {
        uint64_t nb = balloc(fs, pblk);
        if (!nb) { kfree(buf); return -1; }
        memset(buf, 0, fs->bs);
        w16(buf, EXT_MAGIC); w16(buf + 2, 1); w16(buf + 4, (fs->bs - 12 - (fs->csum ? 4 : 0)) / 12); w16(buf + 6, 0);
        uint8_t *e = buf + 12;
        w32(e, (uint32_t)lblk); w16(e + 4, 1); w16(e + 6, (uint32_t)(pblk >> 32)); w32(e + 8, (uint32_t)pblk);
        uint16_t rn = r16(root + 2);
        uint8_t *ix = root + 12 + 12 * rn;
        w32(ix, (uint32_t)lblk); w32(ix + 4, (uint32_t)nb); w16(ix + 8, (uint32_t)(nb >> 32)); w16(ix + 10, 0);
        w16(root + 2, rn + 1);
        iblocks_add(fs, in, 1);
        leaf_blk = nb;
    }
done:
    if (depth == 1 || leaf_blk) {
        ext_tail_csum(fs, ino, in, leaf);
        int rc = bwrite(fs, leaf_blk, leaf);
        kfree(buf);
        return rc;
    }
    return 0;
}

/* Every block of the file (and of its extent tree) back to the free pool. */
static void free_extents(struct ext4 *fs, const uint8_t *node, int level)
{
    if (r16(node) != EXT_MAGIC || level > 5) return;
    uint16_t n = r16(node + 2), depth = r16(node + 6);
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *e = node + 12 + 12 * i;
        if (!depth) {
            uint32_t len = r16(e + 4);
            if (len > EXT_INIT_MAX_LEN) len -= EXT_INIT_MAX_LEN;
            for (uint32_t k = 0; k < len; k++) bfree(fs, ext_start(e) + k);
            continue;
        }
        uint8_t *buf = kmalloc(fs->bs);
        if (buf && !bread(fs, idx_leaf(e), buf)) free_extents(fs, buf, level + 1);
        kfree(buf);
        bfree(fs, idx_leaf(e));
    }
}

static void free_indirect(struct ext4 *fs, uint64_t b, int level)
{
    if (!b) return;
    if (level) {
        uint8_t *buf = kmalloc(fs->bs);
        if (buf && !bread(fs, b, buf))
            for (uint32_t i = 0; i < fs->bs / 4; i++) free_indirect(fs, r32(buf + 4 * i), level - 1);
        kfree(buf);
    }
    bfree(fs, b);
}

static void free_all_blocks(struct ext4 *fs, uint8_t *in)
{
    uint32_t mode = r16(in + I_MODE) & EXT_S_IFMT;
    if (r32(in + I_FLAGS) & FL_INLINE_DATA) return;
    if (r32(in + I_FLAGS) & FL_EXTENTS) free_extents(fs, in + I_BLOCK, 0);
    else if (mode == EXT_S_IFREG || mode == EXT_S_IFDIR) {
        for (int i = 0; i < 12; i++) bfree(fs, r32(in + I_BLOCK + 4 * i));
        for (int l = 1; l <= 3; l++) free_indirect(fs, r32(in + I_BLOCK + 4 * (11 + l)), l);
    }
    memset(in + I_BLOCK, 0, 60);
    w16(in + I_BLOCK, EXT_MAGIC); w16(in + I_BLOCK + 4, 4);
    w32(in + I_FLAGS, (r32(in + I_FLAGS) | FL_EXTENTS) & ~FL_INDEX);
    w32(in + I_BLOCKS_LO, 0); w16(in + I_BLOCKS_HI, 0);
}

/* ---- file data ------------------------------------------------------------------------ */

static long ext4_read(struct vnode *n, uint64_t pos, void *buf, size_t len)
{
    struct ext4 *fs = EFS(n);
    lock(fs);
    uint8_t *in = fs->ibuf;
    long rc = -1;
    if (iread(fs, INO(n), in)) goto out;
    uint64_t size = isize(in);
    if (r32(in + I_FLAGS) & FL_INLINE_DATA) {           /* small files kept in the inode */
        if (pos >= size) { rc = 0; goto out; }
        if (len > size - pos) len = (size_t)(size - pos);
        if (pos + len > 60) len = pos < 60 ? (size_t)(60 - pos) : 0;
        memcpy(buf, in + I_BLOCK + pos, len);
        rc = (long)len;
        goto out;
    }
    if (pos >= size) { rc = 0; goto out; }
    if (len > size - pos) len = (size_t)(size - pos);
    size_t done = 0;
    while (done < len) {
        uint64_t lb = (pos + done) / fs->bs;
        uint32_t in_blk = (uint32_t)((pos + done) % fs->bs);
        size_t chunk = fs->bs - in_blk;
        if (chunk > len - done) chunk = len - done;
        uint64_t pb = bmap(fs, in, lb);
        if (!pb) memset((uint8_t *)buf + done, 0, chunk);
        else if (in_blk == 0 && chunk == fs->bs) {
            /* a run of blocks that follow each other on the disk goes at once */
            uint32_t run = 1;
            while (done + (size_t)(run + 1) * fs->bs <= len && run < 256 && bmap(fs, in, lb + run) == pb + run) run++;
            if (blkdev_read_bytes(fs->dev, pb * fs->bs, (uint8_t *)buf + done, (size_t)run * fs->bs) != (long)((size_t)run * fs->bs)) break;
            chunk = (size_t)run * fs->bs;
        } else {
            if (bread(fs, pb, fs->blk)) break;
            memcpy((uint8_t *)buf + done, fs->blk + in_blk, chunk);
        }
        done += chunk;
    }
    rc = done ? (long)done : -1;
out:
    unlock(fs);
    return rc;
}

/* Blocks for [pos, pos+len) of an extents file: allocated where missing. */
static int data_write(struct ext4 *fs, uint32_t ino, uint8_t *in, uint64_t pos, const void *buf, size_t len, size_t *written)
{
    size_t done = 0;
    uint64_t goal = 0;
    *written = 0;
    while (done < len) {
        uint64_t lb = (pos + done) / fs->bs;
        uint32_t in_blk = (uint32_t)((pos + done) % fs->bs);
        size_t chunk = fs->bs - in_blk;
        if (chunk > len - done) chunk = len - done;
        uint64_t pb = bmap(fs, in, lb);
        int fresh = 0;
        if (!pb) {
            if (!goal && lb) goal = bmap(fs, in, lb - 1) + 1;
            if (!goal) goal = gd64(fs, (ino - 1) / fs->ipg, GD_INODE_TABLE, GD_INODE_TABLE_HI);
            pb = balloc(fs, goal);
            if (!pb) return -1;
            if (ext_insert(fs, ino, in, lb, pb)) { bfree(fs, pb); return -1; }
            iblocks_add(fs, in, 1);
            fresh = 1;
        }
        goal = pb + 1;
        if (in_blk == 0 && chunk == fs->bs) {
            if (bwrite(fs, pb, (const uint8_t *)buf + done)) return -1;
        } else {
            if (fresh) memset(fs->blk, 0, fs->bs);
            else if (bread(fs, pb, fs->blk)) return -1;
            memcpy(fs->blk + in_blk, (const uint8_t *)buf + done, chunk);
            if (bwrite(fs, pb, fs->blk)) return -1;
        }
        done += chunk;
        *written = done;
    }
    return 0;
}

static int flush_meta(struct ext4 *fs);

static int writable(struct ext4 *fs)
{
    if (fs->rw) return 1;
    if (!fs->ro_reason_logged++) kprintf("ext4: %s is mounted read-only\n", fs->dev->name);
    return 0;
}

static long ext4_write(struct vnode *n, uint64_t pos, const void *buf, size_t len)
{
    struct ext4 *fs = EFS(n);
    if (!writable(fs)) return -1;
    lock(fs);
    uint8_t *in = fs->ibuf;
    long rc = -1;
    size_t done = 0;
    if (iread(fs, INO(n), in)) goto out;
    if (!(r32(in + I_FLAGS) & FL_EXTENTS) || (r32(in + I_FLAGS) & FL_INLINE_DATA)) {
        if (isize(in) == 0) free_all_blocks(fs, in);         /* empty: start it with extents */
        else { kprintf("ext4: inode %u is not in extents: not written\n", INO(n)); goto out; }
    }
    int err = data_write(fs, INO(n), in, pos, buf, len, &done);
    if (pos + done > isize(in)) isize_set(in, pos + done);
    uint32_t t = now();
    w32(in + I_MTIME, t); w32(in + I_CTIME, t);
    if (iwrite(fs, INO(n), in) == 0) {
        n->size = isize(in);
        rc = done ? (long)done : (err ? -1 : 0);
    }
out:
    flush_meta(fs);                                     /* bitmaps and counts as the blocks now are */
    unlock(fs);
    return rc;
}

static int ext4_truncate(struct vnode *n, uint64_t size)
{
    struct ext4 *fs = EFS(n);
    if (!writable(fs)) return -1;
    lock(fs);
    uint8_t *in = fs->ibuf;
    int rc = -1;
    if (iread(fs, INO(n), in)) goto out;
    if (size == 0) free_all_blocks(fs, in);
    else if (size < isize(in)) goto out;                 /* to empty, or longer (a hole): as the FAT driver */
    isize_set(in, size);
    uint32_t t = now();
    w32(in + I_MTIME, t); w32(in + I_CTIME, t);
    rc = iwrite(fs, INO(n), in);
    if (!rc) n->size = size;
out:
    flush_meta(fs);                                     /* bitmaps and counts as the blocks now are */
    unlock(fs);
    return rc;
}

/* ---- directories ---------------------------------------------------------------------- */

/* An entry: inode, rec_len, name_len, file type, name; with metadata_csum
 * each leaf block ends in a 12-byte fake entry holding its checksum. */
static uint32_t dir_space(struct ext4 *fs) { return fs->bs - (fs->csum ? 12 : 0); }

static int is_tail(const uint8_t *e) { return r32(e) == 0 && r16(e + 4) == 12 && e[6] == 0 && e[7] == 0xde; }

static void dir_tail_set(struct ext4 *fs, uint32_t ino, const uint8_t *in, uint8_t *blk)
{
    if (!fs->csum) return;
    uint8_t *t = blk + fs->bs - 12;
    memset(t, 0, 12);
    w16(t + 4, 12); t[7] = 0xde;
    w32(t + 8, crc32c(inode_seed(fs, ino, in), blk, fs->bs - 12));
}

/* An htree root or index block (they look like one empty entry, or "." and "..") */
static int dx_countlimit(struct ext4 *fs, const uint8_t *blk)
{
    uint32_t rl = r16(blk + 4);
    if (rl == fs->bs && r32(blk) == 0) return 8;
    if (rl == 12 && r16(blk + 12 + 4) == fs->bs - 12 && blk[24 + 5] == 8) return 32;
    return 0;
}

static void dx_csum_set(struct ext4 *fs, uint32_t ino, const uint8_t *in, uint8_t *blk, int off)
{
    if (!fs->csum) return;
    uint16_t limit = r16(blk + off), count = r16(blk + off + 2);
    uint8_t *t = blk + off + 8 * limit;
    uint32_t c = crc32c(inode_seed(fs, ino, in), blk, (size_t)off + 8u * count);
    c = crc32c(c, t, 4);
    c = crc32c(c, "\0\0\0\0", 4);
    w32(t + 4, c);
}

typedef int (*dir_cb)(struct ext4 *fs, uint8_t *blk, uint8_t *e, uint64_t lblk, void *arg);

/* Every entry of every leaf block, in order; cb returning nonzero stops it. */
static int dir_walk(struct ext4 *fs, const uint8_t *in, dir_cb cb, void *arg)
{
    uint64_t nblk = (isize(in) + fs->bs - 1) / fs->bs;
    uint8_t *blk = kmalloc(fs->bs);
    if (!blk) return -1;
    int rc = 0;
    int htree = (r32(in + I_FLAGS) & FL_INDEX) != 0;
    for (uint64_t lb = 0; lb < nblk && !rc; lb++) {
        uint64_t pb = bmap(fs, in, lb);
        if (!pb || bread(fs, pb, blk)) continue;
        if (htree && dx_countlimit(fs, blk) && lb == 0) {         /* the root: "." and ".." only */
            rc = cb(fs, blk, blk, lb, arg);
            if (!rc) rc = cb(fs, blk, blk + 12, lb, arg);
            continue;
        }
        if (htree && dx_countlimit(fs, blk) == 8) continue;        /* an index node */
        for (uint32_t off = 0; off + 8 <= fs->bs && !rc; ) {
            uint8_t *e = blk + off;
            uint32_t rl = r16(e + 4);
            if (rl < 8 || off + rl > fs->bs) break;
            if (!is_tail(e) && r32(e)) rc = cb(fs, blk, e, lb, arg);
            off += rl;
        }
    }
    kfree(blk);
    return rc;
}

struct find_arg { const char *name; size_t len; uint32_t ino; uint8_t type; };

static int find_cb(struct ext4 *fs, uint8_t *blk, uint8_t *e, uint64_t lblk, void *arg)
{
    (void)fs; (void)blk; (void)lblk;
    struct find_arg *a = arg;
    if (e[6] != a->len || memcmp(e + 8, a->name, a->len)) return 0;
    a->ino = r32(e); a->type = e[7];
    return 1;
}

static struct vnode *make_vnode(struct vnode *dir, const char *name, size_t len, uint32_t ino, const uint8_t *in)
{
    int isdir = (r16(in + I_MODE) & EXT_S_IFMT) == EXT_S_IFDIR;
    struct vnode *v = vnode_alloc(name, len, isdir ? VNODE_DIR : VNODE_FILE, dir);
    struct enode *en = kzalloc(sizeof *en);
    if (!v || !en) { kfree(en); if (v) vnode_free(v); return NULL; }
    en->ino = ino;
    v->mnt = dir->mnt; v->ops = dir->ops; v->priv = en; v->ino = ino;
    v->size = isdir ? 0 : isize(in);
    return v;
}

static struct vnode *ext4_lookup(struct vnode *dir, const char *name, size_t len)
{
    struct ext4 *fs = EFS(dir);
    lock(fs);
    struct vnode *v = NULL;
    struct find_arg a = { name, len, 0, 0 };
    if (!iread(fs, INO(dir), fs->ibuf) && dir_walk(fs, fs->ibuf, find_cb, &a) == 1 && !iread(fs, a.ino, fs->ibuf))
        v = make_vnode(dir, name, len, a.ino, fs->ibuf);
    unlock(fs);
    return v;
}

struct readdir_arg { size_t index; struct dirent *out; };

static int readdir_cb(struct ext4 *fs, uint8_t *blk, uint8_t *e, uint64_t lblk, void *arg)
{
    (void)fs; (void)blk; (void)lblk;
    struct readdir_arg *a = arg;
    uint8_t nl = e[6];
    if ((nl == 1 && e[8] == '.') || (nl == 2 && e[8] == '.' && e[9] == '.')) return 0;
    if (a->index--) return 0;
    memset(a->out->name, 0, VFS_NAME_MAX);
    memcpy(a->out->name, e + 8, nl < VFS_NAME_MAX ? nl : VFS_NAME_MAX - 1);
    a->out->type = e[7] == 2 ? VNODE_DIR : VNODE_FILE;
    a->out->ino = r32(e);
    a->out->size = 0;
    return 1;
}

static int ext4_readdir(struct vnode *dir, size_t index, struct dirent *out)
{
    struct ext4 *fs = EFS(dir);
    lock(fs);
    struct readdir_arg a = { index, out };
    int rc = !iread(fs, INO(dir), fs->ibuf) && dir_walk(fs, fs->ibuf, readdir_cb, &a) == 1;
    if (rc && out->type == VNODE_FILE) {
        uint8_t *in = kmalloc(fs->inode_size);
        if (in && !iread(fs, out->ino, in)) out->size = isize(in);
        kfree(in);
    }
    unlock(fs);
    return rc;
}

/* ---- htree: name hashes, as Linux's fs/ext4/hash.c ---------------------------------- */

static uint32_t rol32(uint32_t v, int s) { return v << s | v >> (32 - s); }

static void tea_transform(uint32_t buf[4], const uint32_t in[4])
{
    uint32_t sum = 0, b0 = buf[0], b1 = buf[1], a = in[0], b = in[1], c = in[2], d = in[3];
    for (int n = 16; n; n--) {
        sum += 0x9e3779b9;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0; buf[1] += b1;
}

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROUND(f, a, b, c, d, x, s) (a += f(b, c, d) + x, a = rol32(a, s))
static void half_md4_transform(uint32_t buf[4], const uint32_t in[8])
{
    const uint32_t K2 = 013240474631UL, K3 = 015666365641UL;
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    ROUND(F, a, b, c, d, in[0], 3);  ROUND(F, d, a, b, c, in[1], 7);  ROUND(F, c, d, a, b, in[2], 11); ROUND(F, b, c, d, a, in[3], 19);
    ROUND(F, a, b, c, d, in[4], 3);  ROUND(F, d, a, b, c, in[5], 7);  ROUND(F, c, d, a, b, in[6], 11); ROUND(F, b, c, d, a, in[7], 19);
    ROUND(G, a, b, c, d, in[1] + K2, 3);  ROUND(G, d, a, b, c, in[3] + K2, 5);  ROUND(G, c, d, a, b, in[5] + K2, 9);  ROUND(G, b, c, d, a, in[7] + K2, 13);
    ROUND(G, a, b, c, d, in[0] + K2, 3);  ROUND(G, d, a, b, c, in[2] + K2, 5);  ROUND(G, c, d, a, b, in[4] + K2, 9);  ROUND(G, b, c, d, a, in[6] + K2, 13);
    ROUND(H, a, b, c, d, in[3] + K3, 3);  ROUND(H, d, a, b, c, in[7] + K3, 9);  ROUND(H, c, d, a, b, in[2] + K3, 11); ROUND(H, b, c, d, a, in[6] + K3, 15);
    ROUND(H, a, b, c, d, in[1] + K3, 3);  ROUND(H, d, a, b, c, in[5] + K3, 9);  ROUND(H, c, d, a, b, in[0] + K3, 11); ROUND(H, b, c, d, a, in[4] + K3, 15);
    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}
#undef F
#undef G
#undef H
#undef ROUND

static void str2hashbuf(const char *msg, int len, uint32_t *buf, int num, int uns)
{
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8), val;
    pad |= pad << 16;
    if (len > num * 4) len = num * 4;
    for (; len >= 4; len -= 4, msg += 4, num--) {
        if (uns) val = (uint32_t)(uint8_t)msg[0] << 24 | (uint32_t)(uint8_t)msg[1] << 16 | (uint32_t)(uint8_t)msg[2] << 8 | (uint8_t)msg[3];
        else val = ((uint32_t)(int32_t)(int8_t)msg[0] << 24) + ((uint32_t)(int32_t)(int8_t)msg[1] << 16) +
                   ((uint32_t)(int32_t)(int8_t)msg[2] << 8) + (uint32_t)(int32_t)(int8_t)msg[3];
        *buf++ = val;
    }
    val = pad;
    for (int i = 0; i < len; i++) val = (uns ? (uint32_t)(uint8_t)msg[i] : (uint32_t)(int32_t)(int8_t)msg[i]) + (val << 8);
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}

static uint32_t dx_hack_hash(const char *name, int len, int uns)
{
    uint32_t hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
    while (len--) {
        int c = uns ? (int)(uint8_t)*name++ : (int)(int8_t)*name++;
        hash = hash1 + (hash0 ^ (uint32_t)(c * 7152373));
        if (hash & 0x80000000) hash -= 0x7fffffff;
        hash1 = hash0; hash0 = hash;
    }
    return hash0 << 1;
}

/* Versions 0 legacy, 1 half_md4, 2 tea; +3 their unsigned forms. -1 if unknown. */
static int dirhash(struct ext4 *fs, int version, const char *name, int len, uint32_t *hash)
{
    uint32_t buf[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, in[8], h;
    if (fs->hash_seed[0] | fs->hash_seed[1] | fs->hash_seed[2] | fs->hash_seed[3]) memcpy(buf, fs->hash_seed, sizeof buf);
    int uns = version >= 3;
    switch (version) {
    case 0: case 3: h = dx_hack_hash(name, len, uns); break;
    case 1: case 4:
        for (const char *p = name; len > 0; len -= 32, p += 32) { str2hashbuf(p, len, in, 8, uns); half_md4_transform(buf, in); }
        h = buf[1];
        break;
    case 2: case 5:
        for (const char *p = name; len > 0; len -= 16, p += 16) { str2hashbuf(p, len, in, 4, uns); tea_transform(buf, in); }
        h = buf[0];
        break;
    default: return -1;
    }
    h &= ~1u;
    if (h == (0x7fffffffu << 1)) h = (0x7fffffffu - 1) << 1;
    *hash = h;
    return 0;
}

/* ---- adding and removing entries -------------------------------------------------------- */

static uint32_t rec_need(uint32_t name_len) { return (8 + name_len + 3) & ~3u; }

/* Room for a new entry in this leaf block: an entry with slack after it,
 * or an unused one. Writes it and returns 0, else 1 (no room). */
static int leaf_insert(struct ext4 *fs, uint8_t *blk, const char *name, size_t len, uint32_t ino, uint8_t type)
{
    uint32_t need = rec_need((uint32_t)len), end = dir_space(fs);
    for (uint32_t off = 0; off + 8 <= end; ) {
        uint8_t *e = blk + off;
        uint32_t rl = r16(e + 4);
        if (rl < 8 || off + rl > end) return 1;
        uint32_t used = r32(e) ? rec_need(e[6]) : 0;
        if (rl >= used + need) {
            uint8_t *ne = e;
            if (used) { w16(e + 4, used); ne = e + used; }
            w32(ne, ino); w16(ne + 4, rl - used); ne[6] = (uint8_t)len; ne[7] = type;
            memcpy(ne + 8, name, len);
            return 0;
        }
        off += rl;
    }
    return 1;
}

/* A new block at the end of a directory, empty or holding one entry. */
static int dir_grow(struct ext4 *fs, uint32_t dino, uint8_t *din, uint8_t *blk, uint64_t *lblk, uint64_t *pblk)
{
    uint64_t lb = isize(din) / fs->bs, goal = lb ? bmap(fs, din, lb - 1) + 1 : 0;
    uint64_t pb = balloc(fs, goal);
    if (!pb) return -1;
    if (ext_insert(fs, dino, din, lb, pb)) { bfree(fs, pb); return -1; }
    iblocks_add(fs, din, 1);
    isize_set(din, (lb + 1) * fs->bs);
    memset(blk, 0, fs->bs);
    w16(blk + 4, dir_space(fs));
    *lblk = lb; *pblk = pb;
    return 0;
}

struct hent { uint32_t hash; uint32_t off; };

/* An htree directory: find the leaf for the name's hash and put it there;
 * a full leaf is split by hash into a new block, its upper half moving. */
static int dx_insert(struct ext4 *fs, uint32_t dino, uint8_t *din, const char *name, size_t len, uint32_t ino, uint8_t type)
{
    uint8_t *root = kmalloc(fs->bs), *node = kmalloc(fs->bs), *leaf = kmalloc(fs->bs), *nleaf = kmalloc(fs->bs);
    struct hent *h = NULL;
    int rc = -1;
    if (!root || !node || !leaf || !nleaf) goto out;
    uint64_t rootpb = bmap(fs, din, 0);
    if (!rootpb || bread(fs, rootpb, root) || dx_countlimit(fs, root) != 32) goto out;
    int version = root[24 + 4], levels = root[24 + 6];
    if (version <= 2 && (r32(fs->sb + SB_FLAGS) & 2)) version += 3;    /* the filesystem's unsigned char hashes */
    uint32_t hash;
    if (levels > 1 || dirhash(fs, version, name, (int)len, &hash)) goto out;
    /* down to the leaf: the last entry whose hash is <= ours (entry 0 covers 0) */
    uint8_t *ix = root;
    int ix_off = 32;
    uint64_t ix_pb = rootpb, ix_lb = 0;
    uint8_t *pick = NULL;
    for (int level = 0; ; level++) {
        uint16_t count = r16(ix + ix_off + 2);
        pick = ix + ix_off;
        for (uint16_t i = 1; i < count; i++) if (r32(ix + ix_off + 8 * i) <= hash) pick = ix + ix_off + 8 * i;
        if (level == levels) break;
        ix_lb = r32(pick + 4);
        ix_pb = bmap(fs, din, ix_lb);
        if (!ix_pb || bread(fs, ix_pb, node)) goto out;
        ix = node; ix_off = 8;
    }
    uint64_t leaf_lb = r32(pick + 4), leaf_pb = bmap(fs, din, leaf_lb);
    if (!leaf_pb || bread(fs, leaf_pb, leaf)) goto out;
    if (leaf_insert(fs, leaf, name, len, ino, type) == 0) {
        dir_tail_set(fs, dino, din, leaf);
        rc = bwrite(fs, leaf_pb, leaf);
        goto out;
    }
    /* split: the index needs room for one more */
    uint16_t limit = r16(ix + ix_off), count = r16(ix + ix_off + 2);
    if (count >= limit) { kprintf("ext4: directory %u: its htree index is full\n", dino); goto out; }
    uint32_t nent = 0, end = dir_space(fs);
    h = kmalloc(sizeof *h * (fs->bs / 12 + 1));
    if (!h) goto out;
    for (uint32_t off = 0; off + 8 <= end; ) {
        uint8_t *e = leaf + off;
        uint32_t rl = r16(e + 4);
        if (rl < 8 || off + rl > end) break;
        if (r32(e) && dirhash(fs, version, (const char *)e + 8, e[6], &h[nent].hash) == 0) h[nent++].off = off;
        off += rl;
    }
    for (uint32_t i = 1; i < nent; i++)                         /* by hash */
        for (uint32_t j = i; j > 0 && h[j - 1].hash > h[j].hash; j--) { struct hent t = h[j]; h[j] = h[j - 1]; h[j - 1] = t; }
    uint32_t mid = nent / 2;
    while (mid > 0 && mid < nent && h[mid].hash == h[mid - 1].hash) mid++;   /* equal hashes stay together */
    if (mid == 0 || mid >= nent) { kprintf("ext4: directory %u: a leaf of one hash is full\n", dino); goto out; }
    uint32_t split = h[mid].hash;
    uint64_t new_lb, new_pb;
    if (dir_grow(fs, dino, din, nleaf, &new_lb, &new_pb)) goto out;
    /* rebuild both leaves from the sorted entries */
    uint8_t *old = kmalloc(fs->bs);
    if (!old) goto out;
    memcpy(old, leaf, fs->bs);
    memset(leaf, 0, fs->bs);
    w16(leaf + 4, end);
    for (uint32_t i = 0; i < nent; i++) {
        uint8_t *e = old + h[i].off;
        leaf_insert(fs, i < mid ? leaf : nleaf, (const char *)e + 8, e[6], r32(e), e[7]);
    }
    kfree(old);
    leaf_insert(fs, hash < split ? leaf : nleaf, name, len, ino, type);
    /* the index: (split, new block) after the one we came through */
    uint8_t *at = pick + 8, *last = ix + ix_off + 8 * count;
    memmove(at + 8, at, (size_t)(last - at));
    w32(at, split); w32(at + 4, (uint32_t)new_lb);
    w16(ix + ix_off + 2, count + 1);
    dir_tail_set(fs, dino, din, leaf);
    dir_tail_set(fs, dino, din, nleaf);
    dx_csum_set(fs, dino, din, ix, ix_off);
    if (bwrite(fs, leaf_pb, leaf) || bwrite(fs, new_pb, nleaf) || bwrite(fs, ix_pb, ix)) goto out;
    rc = 0;
out:
    kfree(root); kfree(node); kfree(leaf); kfree(nleaf); kfree(h);
    return rc;
}

/* A name into a directory (whose inode is din, written by the caller). */
static int dir_add(struct ext4 *fs, uint32_t dino, uint8_t *din, const char *name, size_t len, uint32_t ino, uint8_t type)
{
    if (r32(din + I_FLAGS) & FL_INDEX) return dx_insert(fs, dino, din, name, len, ino, type);
    uint64_t nblk = isize(din) / fs->bs;
    uint8_t *blk = kmalloc(fs->bs);
    if (!blk) return -1;
    int rc = -1;
    for (uint64_t lb = 0; lb < nblk; lb++) {
        uint64_t pb = bmap(fs, din, lb);
        if (!pb || bread(fs, pb, blk)) continue;
        if (leaf_insert(fs, blk, name, len, ino, type) == 0) {
            dir_tail_set(fs, dino, din, blk);
            rc = bwrite(fs, pb, blk);
            goto out;
        }
    }
    uint64_t lb, pb;
    if (dir_grow(fs, dino, din, blk, &lb, &pb) == 0 && leaf_insert(fs, blk, name, len, ino, type) == 0) {
        dir_tail_set(fs, dino, din, blk);
        rc = bwrite(fs, pb, blk);
    }
out:
    kfree(blk);
    return rc;
}

struct remove_arg { const char *name; size_t len; uint32_t dino; uint8_t *din; int done; };

static int remove_cb(struct ext4 *fs, uint8_t *blk, uint8_t *e, uint64_t lblk, void *arg)
{
    struct remove_arg *a = arg;
    if (e[6] != a->len || memcmp(e + 8, a->name, a->len)) return 0;
    /* fold into the entry before it in the block, or mark it unused */
    uint8_t *prev = NULL;
    for (uint32_t off = 0; off < (uint32_t)(e - blk); off += r16(blk + off + 4)) {
        if (r16(blk + off + 4) < 8) break;
        prev = blk + off;
    }
    if (prev && prev + r16(prev + 4) == e) w16(prev + 4, r16(prev + 4) + r16(e + 4));
    else w32(e, 0);
    dir_tail_set(fs, a->dino, a->din, blk);
    a->done = bwrite(fs, bmap(fs, a->din, lblk), blk) == 0 ? 1 : -1;
    return 1;
}

static int dir_remove(struct ext4 *fs, uint32_t dino, uint8_t *din, const char *name, size_t len)
{
    struct remove_arg a = { name, len, dino, din, 0 };
    dir_walk(fs, din, remove_cb, &a);
    return a.done == 1 ? 0 : -1;
}

/* ---- create, unlink, rename ------------------------------------------------------------ */

static void new_inode(struct ext4 *fs, uint8_t *in, int dir)
{
    memset(in, 0, fs->inode_size);
    uint32_t t = now();
    w16(in + I_MODE, dir ? (EXT_S_IFDIR | 0755) : (EXT_S_IFREG | 0644));
    w32(in + I_ATIME, t); w32(in + I_CTIME, t); w32(in + I_MTIME, t);
    w16(in + I_LINKS, dir ? 2 : 1);
    w32(in + I_FLAGS, FL_EXTENTS);
    w16(in + I_BLOCK, EXT_MAGIC); w16(in + I_BLOCK + 4, 4);
    w32(in + I_GENERATION, ++fs->next_gen);
    if (fs->inode_size > 128) {
        uint32_t extra = r16(fs->sb + SB_WANT_EXTRA_ISIZE);
        if (extra < 32) extra = 32;
        if (128 + extra > fs->inode_size) extra = fs->inode_size - 128;
        w16(in + I_EXTRA_ISIZE, extra);
        if (has_extra(fs, in, I_CRTIME + 4)) w32(in + I_CRTIME, t);
    }
}

static struct vnode *ext4_create(struct vnode *dir, const char *name, size_t len, enum vnode_type type)
{
    struct ext4 *fs = EFS(dir);
    if (!writable(fs) || len > 255 || (type != VNODE_FILE && type != VNODE_DIR)) return NULL;
    lock(fs);
    struct vnode *v = NULL;
    uint32_t dino = INO(dir), ino = 0;
    int isdir = type == VNODE_DIR;
    uint8_t *din = kmalloc(fs->inode_size), *in = fs->ibuf;
    if (!din || iread(fs, dino, din)) goto out;
    struct find_arg fa = { name, len, 0, 0 };
    if (dir_walk(fs, din, find_cb, &fa) == 1) goto out;           /* already there */
    if (!(ino = ialloc(fs, dino, isdir))) goto out;
    new_inode(fs, in, isdir);
    if (isdir) {                                                   /* "." and ".." */
        uint8_t *blk = kzalloc(fs->bs);
        uint64_t pb = blk ? balloc(fs, gd64(fs, (ino - 1) / fs->ipg, GD_INODE_TABLE, GD_INODE_TABLE_HI)) : 0;
        if (!pb || ext_insert(fs, ino, in, 0, pb)) { kfree(blk); if (pb) bfree(fs, pb); ifree(fs, ino, 1); goto out; }
        w32(blk, ino); w16(blk + 4, 12); blk[6] = 1; blk[7] = 2; blk[8] = '.';
        w32(blk + 12, dino); w16(blk + 16, dir_space(fs) - 12); blk[18] = 2; blk[19] = 2; blk[20] = '.'; blk[21] = '.';
        isize_set(in, fs->bs);
        iblocks_add(fs, in, 1);
        dir_tail_set(fs, ino, in, blk);
        int rc = bwrite(fs, pb, blk);
        kfree(blk);
        if (rc) goto out;
    }
    if (iwrite(fs, ino, in)) goto out;
    if (dir_add(fs, dino, din, name, len, ino, isdir ? 2 : 1)) {
        free_all_blocks(fs, in);
        ifree(fs, ino, isdir);
        goto out;
    }
    if (isdir) w16(din + I_LINKS, r16(din + I_LINKS) + 1);
    uint32_t t = now();
    w32(din + I_MTIME, t); w32(din + I_CTIME, t);
    if (iwrite(fs, dino, din)) goto out;
    v = make_vnode(dir, name, len, ino, in);
out:
    flush_meta(fs);                                     /* bitmaps and counts as the blocks now are */
    kfree(din);
    unlock(fs);
    return v;
}

static int ext4_unlink(struct vnode *dir, struct vnode *n)
{
    struct ext4 *fs = EFS(dir);
    if (!writable(fs)) return -1;
    lock(fs);
    int rc = -1;
    uint8_t *din = kmalloc(fs->inode_size), *in = fs->ibuf;
    uint32_t ino = INO(n);
    if (!din || iread(fs, INO(dir), din) || iread(fs, ino, in)) goto out;
    int isdir = (r16(in + I_MODE) & EXT_S_IFMT) == EXT_S_IFDIR;
    if (dir_remove(fs, INO(dir), din, n->name, strlen(n->name))) goto out;
    uint32_t t = now();
    if (isdir) w16(din + I_LINKS, r16(din + I_LINKS) - 1);
    w32(din + I_MTIME, t); w32(din + I_CTIME, t);
    if (iwrite(fs, INO(dir), din)) goto out;
    uint16_t links = r16(in + I_LINKS);
    links = isdir ? 0 : links - 1;
    w16(in + I_LINKS, links);
    w32(in + I_CTIME, t);
    if (!links) {
        free_all_blocks(fs, in);
        isize_set(in, 0);
        w32(in + I_DTIME, t);
        ifree(fs, ino, isdir);
    }
    rc = iwrite(fs, ino, in);
    if (!rc) kfree(n->priv), n->priv = NULL;
out:
    flush_meta(fs);                                     /* bitmaps and counts as the blocks now are */
    kfree(din);
    unlock(fs);
    return rc;
}

static int ext4_rename(struct vnode *olddir, struct vnode *n, struct vnode *newdir, const char *name, size_t len)
{
    struct ext4 *fs = EFS(olddir);
    if (!writable(fs) || len > 255) return -1;
    lock(fs);
    int rc = -1;
    uint32_t ino = INO(n), od = INO(olddir), nd = INO(newdir);
    uint8_t *odin = kmalloc(fs->inode_size), *ndin = kmalloc(fs->inode_size), *in = fs->ibuf;
    if (!odin || !ndin || iread(fs, ino, in) || iread(fs, nd, ndin)) goto out;
    int isdir = (r16(in + I_MODE) & EXT_S_IFMT) == EXT_S_IFDIR;
    if (dir_add(fs, nd, ndin, name, len, ino, isdir ? 2 : 1)) goto out;
    if (isdir && od != nd) w16(ndin + I_LINKS, r16(ndin + I_LINKS) + 1);
    if (iwrite(fs, nd, ndin) || iread(fs, od, odin)) goto out;
    if (dir_remove(fs, od, odin, n->name, strlen(n->name))) goto out;
    if (isdir && od != nd) w16(odin + I_LINKS, r16(odin + I_LINKS) - 1);
    if (iwrite(fs, od, odin)) goto out;
    if (isdir && od != nd) {                                       /* its ".." follows */
        uint8_t *blk = kmalloc(fs->bs);
        uint64_t pb = bmap(fs, in, 0);
        if (blk && pb && !bread(fs, pb, blk)) {
            uint8_t *e = blk + r16(blk + 4);                     /* ".." comes after "." */
            if (e[6] == 2 && e[8] == '.' && e[9] == '.') w32(e, nd);
            if (dx_countlimit(fs, blk) == 32) dx_csum_set(fs, ino, in, blk, 32);
            else dir_tail_set(fs, ino, in, blk);
            bwrite(fs, pb, blk);
        }
        kfree(blk);
    }
    w32(in + I_CTIME, now());
    rc = iwrite(fs, ino, in);
out:
    flush_meta(fs);                                     /* bitmaps and counts as the blocks now are */
    kfree(odin); kfree(ndin);
    unlock(fs);
    return rc;
}

/* ---- sync, mount ---------------------------------------------------------------------- */

static void sb_csum(struct ext4 *fs)
{
    if (fs->csum) w32(fs->sb + SB_CHECKSUM, crc32c(0xffffffff, fs->sb, SB_CHECKSUM));
}

static int sb_write(struct ext4 *fs)
{
    sb_csum(fs);
    return blkdev_write_bytes(fs->dev, 1024, fs->sb, 1024) == 1024 ? 0 : -1;
}

/* Where group g's descriptor lives on the disk (meta_bg spreads them out). */
static uint64_t gd_block(struct ext4 *fs, uint32_t g)
{
    uint32_t per = fs->bs / fs->desc_size, b = g / per;
    if (!fs->meta_bg || b < fs->first_meta_bg) return fs->first_data + 1 + b;
    uint32_t first = b * per;
    return group_first(fs, first) + (has_super(fs, first) ? 1 : 0);
}

static int flush_meta(struct ext4 *fs)
{
    if (!fs->rw) return 0;
    int rc = 0;
    for (uint32_t g = 0; g < fs->groups; g++) {
        if (fs->bbm_dirty[g] && fs->bbm[g]) {
            if (fs->csum) {
                uint32_t c = crc32c(fs->seed, fs->bbm[g], fs->bpg / 8);
                w16(gd(fs, g) + GD_BBITMAP_CSUM, c & 0xffff);
                if (fs->desc_size >= 64) w16(gd(fs, g) + GD_BBITMAP_CSUM_HI, c >> 16);
                fs->gd_dirty[g] = 1;
            }
            rc |= bwrite(fs, gd64(fs, g, GD_BLOCK_BITMAP, GD_BLOCK_BITMAP_HI), fs->bbm[g]);
            fs->bbm_dirty[g] = 0;
        }
        if (fs->ibm_dirty[g] && fs->ibm[g]) {
            if (fs->csum) {
                uint32_t c = crc32c(fs->seed, fs->ibm[g], fs->ipg / 8);
                w16(gd(fs, g) + GD_IBITMAP_CSUM, c & 0xffff);
                if (fs->desc_size >= 64) w16(gd(fs, g) + GD_IBITMAP_CSUM_HI, c >> 16);
                fs->gd_dirty[g] = 1;
            }
            rc |= bwrite(fs, gd64(fs, g, GD_INODE_BITMAP, GD_INODE_BITMAP_HI), fs->ibm[g]);
            fs->ibm_dirty[g] = 0;
        }
    }
    uint32_t per = fs->bs / fs->desc_size;
    for (uint32_t g = 0; g < fs->groups; g++) {
        if (!fs->gd_dirty[g]) continue;
        uint32_t first = g / per * per;                          /* the whole block of descriptors */
        for (uint32_t k = first; k < first + per && k < fs->groups; k++) { gd_csum(fs, k); fs->gd_dirty[k] = 0; }
        uint8_t *blk = fs->blk2;
        memset(blk, 0, fs->bs);
        uint32_t n = fs->groups - first < per ? fs->groups - first : per;
        memcpy(blk, gd(fs, first), (size_t)n * fs->desc_size);
        rc |= bwrite(fs, gd_block(fs, first), blk);
    }
    if (fs->sb_dirty) { rc |= sb_write(fs); fs->sb_dirty = 0; }
    if (fs->dev->flush) fs->dev->flush(fs->dev);
    return rc ? -1 : 0;
}

/* Data and metadata out, and the filesystem clean until the next change. */
static int ext4_sync(struct vnode *n)
{
    struct ext4 *fs = EFS(n);
    if (!fs->rw) return 0;
    lock(fs);
    int rc = flush_meta(fs);
    unlock(fs);
    return rc;
}

static const struct vnode_ops ext4_ops = {
    .lookup = ext4_lookup, .create = ext4_create, .unlink = ext4_unlink, .rename = ext4_rename,
    .read = ext4_read, .write = ext4_write, .truncate = ext4_truncate, .readdir = ext4_readdir, .sync = ext4_sync,
};

static void ext4_free(struct ext4 *fs)
{
    if (!fs) return;
    for (uint32_t g = 0; fs->bbm && g < fs->groups; g++) { kfree(fs->bbm[g]); kfree(fs->ibm[g]); }
    kfree(fs->bbm); kfree(fs->ibm); kfree(fs->bbm_dirty); kfree(fs->ibm_dirty); kfree(fs->gd_dirty);
    kfree(fs->gdt); kfree(fs->blk); kfree(fs->blk2); kfree(fs->blk3); kfree(fs->ibuf);
    kfree(fs);
}

static struct vnode *ext4_mount(struct vfs_mount *mnt, struct vnode *devnode, const char *opts)
{
    struct blkdev *dev = blkdev_from_vnode(devnode);
    if (!dev) { kprintf("ext4: not a block device\n"); return NULL; }
    crc_init();
    struct ext4 *fs = kzalloc(sizeof *fs);
    if (!fs) return NULL;
    fs->dev = dev;
    if (blkdev_read_bytes(dev, 1024, fs->sb, 1024) != 1024 || r16(fs->sb + SB_MAGIC) != 0xef53) {
        kprintf("ext4: %s: no ext2/3/4 superblock\n", dev->name);
        goto bad;
    }
    uint32_t incompat = r32(fs->sb + SB_FEATURE_INCOMPAT), ro = r32(fs->sb + SB_FEATURE_RO), compat = r32(fs->sb + SB_FEATURE_COMPAT);
    if (incompat & ~INCOMPAT_READ_OK) { kprintf("ext4: %s: features %x it cannot read\n", dev->name, incompat & ~INCOMPAT_READ_OK); goto bad; }
    fs->bs = 1024u << r32(fs->sb + SB_LOG_BLOCK_SIZE);
    if (fs->bs > 65536) goto bad;
    fs->bpg = r32(fs->sb + SB_BLOCKS_PER_GROUP);
    fs->ipg = r32(fs->sb + SB_INODES_PER_GROUP);
    fs->first_data = r32(fs->sb + SB_FIRST_DATA_BLOCK);
    fs->inode_size = r32(fs->sb + SB_REV_LEVEL) ? r16(fs->sb + SB_INODE_SIZE) : 128;
    fs->bit64 = (incompat & INCOMPAT_64BIT) != 0;
    fs->desc_size = fs->bit64 ? r16(fs->sb + SB_DESC_SIZE) : 32;
    fs->blocks = r32(fs->sb + SB_BLOCKS_LO) | (fs->bit64 ? (uint64_t)r32(fs->sb + SB_BLOCKS_HI) << 32 : 0);
    if (!fs->bpg || !fs->ipg || fs->desc_size < 32 || fs->inode_size < 128 || fs->inode_size > fs->bs) goto bad;
    fs->groups = (uint32_t)((fs->blocks - fs->first_data + fs->bpg - 1) / fs->bpg);
    fs->gdt_blocks = (fs->groups * fs->desc_size + fs->bs - 1) / fs->bs;
    fs->reserved_gdt = r16(fs->sb + SB_RESERVED_GDT);
    fs->meta_bg = (incompat & INCOMPAT_META_BG) != 0;
    fs->first_meta_bg = r32(fs->sb + SB_FIRST_META_BG);
    fs->sparse = (ro & 0x1) != 0;
    fs->csum = (ro & RO_COMPAT_METADATA_CSUM) != 0;
    fs->gdt_csum = (ro & RO_COMPAT_GDT_CSUM) != 0;
    fs->seed = (incompat & INCOMPAT_CSUM_SEED) ? r32(fs->sb + SB_CHECKSUM_SEED) : crc32c(0xffffffff, fs->sb + SB_UUID, 16);
    for (int i = 0; i < 4; i++) fs->hash_seed[i] = r32(fs->sb + SB_HASH_SEED + 4 * i);
    fs->next_gen = now() ^ r32(fs->sb + SB_UUID);
    if (fs->csum && crc32c(0xffffffff, fs->sb, SB_CHECKSUM) != r32(fs->sb + SB_CHECKSUM))
        kprintf("ext4: %s: the superblock's checksum is wrong\n", dev->name);

    fs->blk = kmalloc(fs->bs); fs->blk2 = kmalloc(fs->bs); fs->blk3 = kmalloc(fs->bs);
    fs->ibuf = kmalloc(fs->inode_size);
    fs->gdt = kzalloc((size_t)fs->groups * fs->desc_size);
    fs->bbm = kzalloc(sizeof(uint8_t *) * fs->groups); fs->ibm = kzalloc(sizeof(uint8_t *) * fs->groups);
    fs->bbm_dirty = kzalloc(fs->groups); fs->ibm_dirty = kzalloc(fs->groups); fs->gd_dirty = kzalloc(fs->groups);
    if (!fs->blk || !fs->blk2 || !fs->blk3 || !fs->ibuf || !fs->gdt || !fs->bbm || !fs->ibm || !fs->bbm_dirty || !fs->ibm_dirty || !fs->gd_dirty) goto bad;
    uint32_t per = fs->bs / fs->desc_size;
    for (uint32_t g = 0; g < fs->groups; g += per) {
        if (bread(fs, gd_block(fs, g), fs->blk)) { kprintf("ext4: %s: group descriptors unreadable\n", dev->name); goto bad; }
        uint32_t n = fs->groups - g < per ? fs->groups - g : per;
        memcpy(gd(fs, g), fs->blk, (size_t)n * fs->desc_size);
    }

    /* written to only if everything about it is known */
    const char *why = NULL;
    if (opts && strstr(opts, "ro")) why = "asked";
    else if (incompat & ~INCOMPAT_WRITE_OK) why = incompat & INCOMPAT_RECOVER ? "its journal needs replaying (mount it in Linux or e2fsck it first)" : "it has features this cannot write";
    else if (fs->csum && fs->sb[0x175] != 1) why = "an unknown checksum type";
    else if (!dev->write) why = "the device is read-only";
    fs->rw = !why;
    if (fs->rw) {                                       /* not clean while mounted here */
        w16(fs->sb + SB_STATE, r16(fs->sb + SB_STATE) & ~1u);
        w16(fs->sb + SB_MNT_COUNT, r16(fs->sb + SB_MNT_COUNT) + 1);
        w32(fs->sb + SB_MTIME, now());
        if (sb_write(fs)) { fs->rw = 0; why = "the superblock could not be written"; }
        else if (dev->flush) dev->flush(dev);
    }
    if (ro & RO_COMPAT_QUOTA) kprintf("ext4: %s: quota is not kept up to date here (e2fsck fixes it)\n", dev->name);

    if (iread(fs, ROOT_INO, fs->ibuf)) goto bad;
    struct vnode *root = vnode_alloc("", 0, VNODE_DIR, NULL);
    struct enode *en = kzalloc(sizeof *en);
    if (!root || !en) { kfree(en); if (root) vnode_free(root); goto bad; }
    en->ino = ROOT_INO;
    root->mnt = mnt; root->ops = &ext4_ops; root->priv = en; root->ino = ROOT_INO;
    mnt->priv = fs;
    char label[17] = { 0 };
    memcpy(label, fs->sb + SB_VOLUME_NAME, 16);
    kprintf("ext4: mounted %s \"%s\": %llu MiB in %u-byte blocks, %u groups%s%s%s%s\n", dev->name, label,
            fs->blocks * fs->bs >> 20, fs->bs, fs->groups, fs->csum ? ", checksums" : "",
            (compat & COMPAT_DIR_INDEX) ? ", htree" : "", fs->rw ? "" : ", read-only: ", fs->rw ? "" : why);
    return root;
bad:
    ext4_free(fs);
    return NULL;
}

static void drop_tree(struct vnode *n)
{
    for (struct vnode *c = n->children, *next; c; c = next) { next = c->sibling; drop_tree(c); }
    kfree(n->priv);
    vnode_free(n);
}

static int ext4_unmount(struct vfs_mount *mnt)
{
    struct ext4 *fs = mnt->priv;
    lock(fs);
    if (fs->rw) {
        flush_meta(fs);
        w16(fs->sb + SB_STATE, r16(fs->sb + SB_STATE) | 1);     /* clean */
        w32(fs->sb + SB_WTIME, now());
        sb_write(fs);
        if (fs->dev->flush) fs->dev->flush(fs->dev);
    }
    unlock(fs);
    drop_tree(mnt->root);
    ext4_free(fs);
    return 0;
}

struct fs_type ext4_type = { .name = "ext4", .mount = ext4_mount, .unmount = ext4_unmount };
struct fs_type ext3_type = { .name = "ext3", .mount = ext4_mount, .unmount = ext4_unmount };
struct fs_type ext2_type = { .name = "ext2", .mount = ext4_mount, .unmount = ext4_unmount };
