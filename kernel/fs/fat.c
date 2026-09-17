/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* FAT12/16/32 (CONFIG_FAT). Read/write. Directory entries: 8.3 names are
 * created; VFAT long names are read (so files written elsewhere show their
 * real names) but never written. */
#include "fs/vfs.h"
#include "endian.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

struct bpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;
    uint16_t sectors_per_track, heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    /* FAT32 */
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive, nt_flags, boot_sig;
    uint32_t volume_id;
    char     label[11];
    char     fstype[8];
} __attribute__((packed));

struct fat_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt;
    uint8_t  ctime_tenth;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi;
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed));

struct fat_lfn {
    uint8_t  seq;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;
    uint16_t name3[2];
} __attribute__((packed));

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_LABEL  0x08
#define ATTR_DIR    0x10
#define ATTR_LFN    0x0F

struct fat {
    struct blkdev *dev;
    int type;                       /* 12, 16, 32 */
    uint32_t sector_size, cluster_size, sectors_per_cluster;
    uint32_t fat_start, fat_sectors, fats;
    uint32_t root_start, root_sectors;   /* FAT12/16 fixed root directory */
    uint32_t root_cluster;               /* FAT32 */
    uint32_t data_start;                 /* first data sector */
    uint32_t clusters;                   /* count of data clusters */
    uint32_t eoc;                        /* end-of-chain marker to write */
    uint32_t next_free;
    uint8_t *fat_cache;                  /* one sector */
    uint32_t fat_cache_sector;           /* absolute sector, 0 = none */
    uint8_t *sec;                        /* scratch sector */
};

/* Per-vnode: where the directory entry lives and the first cluster. */
struct fnode {
    uint32_t cluster;                    /* first data cluster (0 = none) */
    uint32_t dirent_sector;              /* absolute sector holding the entry */
    uint32_t dirent_off;
    int fixed_root;                      /* FAT12/16 root: not a cluster chain */
};

static struct fat *fs_of(struct vnode *n) { return n->mnt->priv; }
static struct fnode *fn_of(struct vnode *n) { return n->priv; }

/* ---- sectors and the FAT ---------------------------------------------------- */

static int read_sector(struct fat *fs, uint32_t sector, void *buf)
{
    return fs->dev->read(fs->dev, sector, 1, buf);
}

static int write_sector(struct fat *fs, uint32_t sector, const void *buf)
{
    return fs->dev->write(fs->dev, sector, 1, buf);
}

static uint32_t cluster_sector(struct fat *fs, uint32_t cluster)
{
    return fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
}

static int fat_sector_load(struct fat *fs, uint32_t sector)
{
    if (fs->fat_cache_sector == sector)
        return 0;
    if (read_sector(fs, sector, fs->fat_cache) != 0)
        return -1;
    fs->fat_cache_sector = sector;
    return 0;
}

static int fat_sector_store(struct fat *fs)
{
    for (uint32_t i = 0; i < fs->fats; i++)
        if (write_sector(fs, fs->fat_cache_sector + i * fs->fat_sectors, fs->fat_cache) != 0)
            return -1;
    return 0;
}

/* Read a FAT entry; returns 0xFFFFFFFF on error. Values >= eoc are chain ends. */
static uint32_t fat_get(struct fat *fs, uint32_t cluster)
{
    uint32_t off = fs->type == 12 ? cluster + cluster / 2 : fs->type == 16 ? cluster * 2 : cluster * 4;
    uint32_t sector = fs->fat_start + off / fs->sector_size, in = off % fs->sector_size;
    if (fat_sector_load(fs, sector) != 0)
        return 0xFFFFFFFF;
    uint32_t v;
    if (fs->type == 12) {
        uint32_t lo = fs->fat_cache[in], hi;
        if (in + 1 < fs->sector_size) hi = fs->fat_cache[in + 1];
        else {
            uint8_t save = fs->fat_cache[0]; (void)save;
            uint8_t *tmp = fs->sec;
            if (read_sector(fs, sector + 1, tmp) != 0) return 0xFFFFFFFF;
            hi = tmp[0];
        }
        v = lo | (hi << 8);
        v = (cluster & 1) ? v >> 4 : v & 0xFFF;
        if (v >= 0xFF8) v = 0x0FFFFFFF;
    } else if (fs->type == 16) {
        v = fs->fat_cache[in] | (fs->fat_cache[in + 1] << 8);
        if (v >= 0xFFF8) v = 0x0FFFFFFF;
    } else {
        v = get_le32(fs->fat_cache + in);
        v &= 0x0FFFFFFF;
        if (v >= 0x0FFFFFF8) v = 0x0FFFFFFF;
    }
    return v;
}

static int fat_set(struct fat *fs, uint32_t cluster, uint32_t value)
{
    uint32_t off = fs->type == 12 ? cluster + cluster / 2 : fs->type == 16 ? cluster * 2 : cluster * 4;
    uint32_t sector = fs->fat_start + off / fs->sector_size, in = off % fs->sector_size;
    if (fat_sector_load(fs, sector) != 0)
        return -1;
    if (fs->type == 12) {
        if (in + 1 >= fs->sector_size)
            return -1;                                  /* entry straddling sectors: unsupported for writes */
        uint32_t cur = fs->fat_cache[in] | (fs->fat_cache[in + 1] << 8);
        if (cluster & 1) cur = (cur & 0x000F) | ((value & 0xFFF) << 4);
        else             cur = (cur & 0xF000) | (value & 0xFFF);
        fs->fat_cache[in] = (uint8_t)cur;
        fs->fat_cache[in + 1] = (uint8_t)(cur >> 8);
    } else if (fs->type == 16) {
        fs->fat_cache[in] = (uint8_t)value;
        fs->fat_cache[in + 1] = (uint8_t)(value >> 8);
    } else {
        uint32_t old = get_le32(fs->fat_cache + in);
        put_le32(fs->fat_cache + in, (old & 0xF0000000) | (value & 0x0FFFFFFF));
    }
    return fat_sector_store(fs);
}

static int is_eoc(struct fat *fs, uint32_t v) { (void)fs; return v >= 0x0FFFFFF8 || v == 0xFFFFFFFF; }

static uint32_t alloc_cluster(struct fat *fs)
{
    for (uint32_t n = 0; n < fs->clusters; n++) {
        uint32_t c = 2 + (fs->next_free - 2 + n) % fs->clusters;
        if (fat_get(fs, c) == 0) {
            if (fat_set(fs, c, fs->eoc) != 0)
                return 0;
            fs->next_free = c + 1;
            uint8_t *zero = fs->sec;
            memset(zero, 0, fs->sector_size);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
                write_sector(fs, cluster_sector(fs, c) + s, zero);
            return c;
        }
    }
    return 0;
}

static void free_chain(struct fat *fs, uint32_t c)
{
    while (c >= 2 && !is_eoc(fs, c)) {
        uint32_t next = fat_get(fs, c);
        fat_set(fs, c, 0);
        if (c < fs->next_free) fs->next_free = c;
        c = next;
    }
}

/* Cluster number for logical cluster index `idx` of a chain starting at `first`. */
static uint32_t chain_walk(struct fat *fs, uint32_t first, uint32_t idx, int alloc, uint32_t *first_out)
{
    uint32_t c = first;
    if (!c) {
        if (!alloc) return 0;
        c = alloc_cluster(fs);
        if (!c) return 0;
        if (first_out) *first_out = c;
    }
    for (uint32_t i = 0; i < idx; i++) {
        uint32_t next = fat_get(fs, c);
        if (is_eoc(fs, next) || next < 2) {
            if (!alloc) return 0;
            next = alloc_cluster(fs);
            if (!next) return 0;
            fat_set(fs, c, next);
        }
        c = next;
    }
    return c;
}

/* ---- directory entries ----------------------------------------------------------- */

/* Iterate the sectors of a directory (fixed root or cluster chain). Returns
 * the absolute sector for logical sector `i`, 0 at the end (alloc extends). */
static uint32_t dir_sector(struct fat *fs, struct fnode *d, uint32_t i, int alloc)
{
    if (d->fixed_root)
        return i < fs->root_sectors ? fs->root_start + i : 0;
    uint32_t c = chain_walk(fs, d->cluster, i / fs->sectors_per_cluster, alloc, &d->cluster);
    return c ? cluster_sector(fs, c) + i % fs->sectors_per_cluster : 0;
}

static void name_from_83(const uint8_t *raw, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z' ? raw[i] + 32 : raw[i]);
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z' ? raw[i] + 32 : raw[i]);
    }
    out[n] = '\0';
}

/* Build an 8.3 name; returns 0 if the name cannot be represented. */
static int name_to_83(const char *name, size_t len, uint8_t *raw)
{
    memset(raw, ' ', 11);
    size_t dot = len;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '.') dot = i;
    size_t base = dot, ext = dot < len ? len - dot - 1 : 0;
    if (base == 0 || base > 8 || ext > 3)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (i == dot) continue;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c <= ' ' || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            return 0;
        raw[i < dot ? i : 8 + (i - dot - 1)] = (uint8_t)c;
    }
    return 1;
}

static uint8_t lfn_checksum(const uint8_t *raw)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + raw[i]);
    return sum;
}

/* Walk a directory, calling cb for every real entry with its (possibly long)
 * name. cb returns nonzero to stop; that value is returned. */
struct dir_pos { uint32_t sector, off; };
typedef int (*dir_cb)(struct fat *fs, const struct fat_dirent *e, const char *name, struct dir_pos *pos, void *arg);

static int dir_walk(struct fat *fs, struct fnode *d, dir_cb cb, void *arg)
{
    char lfn[256];
    int lfn_len = 0, lfn_valid = 0;
    uint8_t lfn_sum = 0;
    uint8_t *sec = kmalloc(fs->sector_size);
    if (!sec) return -1;
    int rc = 0;

    for (uint32_t i = 0; ; i++) {
        uint32_t s = dir_sector(fs, d, i, 0);
        if (!s) break;
        if (read_sector(fs, s, sec) != 0) { rc = -1; break; }
        int stop = 0;
        for (uint32_t off = 0; off + 32 <= fs->sector_size; off += 32) {
            struct fat_dirent *e = (void *)(sec + off);
            if (e->name[0] == 0x00) { stop = 1; break; }
            if (e->name[0] == 0xE5) { lfn_valid = 0; continue; }
            if (e->attr == ATTR_LFN) {
                struct fat_lfn *l = (void *)e;
                int seq = l->seq & 0x1F;
                if (l->seq & 0x40) { memset(lfn, 0, sizeof(lfn)); lfn_len = 0; lfn_valid = 1; lfn_sum = l->checksum; }
                if (!lfn_valid || seq < 1 || seq > 20 || l->checksum != lfn_sum) { lfn_valid = 0; continue; }
                uint16_t part[13];
                memcpy(part, l->name1, 10); memcpy(part + 5, l->name2, 12); memcpy(part + 11, l->name3, 4);
                int base = (seq - 1) * 13;
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = le16toh(part[k]);
                    if (ch == 0 || ch == 0xFFFF) { if (base + k > lfn_len) lfn_len = base + k; break; }
                    lfn[base + k] = ch < 128 ? (char)ch : '?';
                    if (base + k + 1 > lfn_len) lfn_len = base + k + 1;
                }
                continue;
            }
            if (e->attr & ATTR_LABEL) { lfn_valid = 0; continue; }
            char name[256];
            if (lfn_valid && lfn_sum == lfn_checksum(e->name)) {
                memcpy(name, lfn, (size_t)lfn_len);
                name[lfn_len] = '\0';
            } else {
                name_from_83(e->name, name);
            }
            lfn_valid = 0;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;
            struct dir_pos pos = { s, off };
            rc = cb(fs, e, name, &pos, arg);
            if (rc) { stop = 1; break; }
        }
        if (stop) break;
    }
    kfree(sec);
    return rc;
}

static int streq_ci(const char *a, const char *b, size_t blen)
{
    size_t i = 0;
    for (; i < blen && a[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return i == blen && a[i] == '\0';
}

struct lookup_arg { const char *name; size_t len; struct fat_dirent e; struct dir_pos pos; int found; };

static int lookup_cb(struct fat *fs, const struct fat_dirent *e, const char *name, struct dir_pos *pos, void *arg)
{
    (void)fs;
    struct lookup_arg *la = arg;
    if (streq_ci(name, la->name, la->len)) {
        la->e = *e;
        la->pos = *pos;
        la->found = 1;
        return 1;
    }
    return 0;
}

static struct vnode *make_vnode(struct vnode *dir, const char *name, size_t len, const struct fat_dirent *e, const struct dir_pos *pos)
{
    struct fnode *fn = kzalloc(sizeof(*fn));
    if (!fn) return NULL;
    fn->cluster = ((uint32_t)le16toh(e->cluster_hi) << 16) | le16toh(e->cluster_lo);
    fn->dirent_sector = pos->sector;
    fn->dirent_off = pos->off;
    struct vnode *n = vnode_alloc(name, len, (e->attr & ATTR_DIR) ? VNODE_DIR : VNODE_FILE, dir);
    if (!n) { kfree(fn); return NULL; }
    n->priv = fn;
    n->size = (e->attr & ATTR_DIR) ? 0 : le32toh(e->size);
    n->ino = fn->cluster ? fn->cluster : (pos->sector << 4 | pos->off / 32);
    return n;
}

static struct vnode *fat_lookup(struct vnode *dir, const char *name, size_t len)
{
    struct lookup_arg la;
    memset(&la, 0, sizeof(la));
    la.name = name;
    la.len = len;
    dir_walk(fs_of(dir), fn_of(dir), lookup_cb, &la);
    return la.found ? make_vnode(dir, name, len, &la.e, &la.pos) : NULL;
}

/* Update the on-disk entry (size, first cluster) of a file. */
static int write_dirent(struct vnode *n)
{
    struct fat *fs = fs_of(n);
    struct fnode *fn = fn_of(n);
    if (!fn->dirent_sector) return 0;           /* the root */
    if (read_sector(fs, fn->dirent_sector, fs->sec) != 0) return -1;
    struct fat_dirent *e = (void *)(fs->sec + fn->dirent_off);
    e->size = htole32(n->type == VNODE_DIR ? 0 : (uint32_t)n->size);
    e->cluster_hi = htole16((uint16_t)(fn->cluster >> 16));
    e->cluster_lo = htole16((uint16_t)fn->cluster);
    return write_sector(fs, fn->dirent_sector, fs->sec);
}

/* Find a free 32-byte slot in a directory, extending it if necessary. */
static int find_free_slot(struct fat *fs, struct fnode *d, struct dir_pos *out)
{
    for (uint32_t i = 0; ; i++) {
        uint32_t s = dir_sector(fs, d, i, 1);
        if (!s) return -1;
        if (read_sector(fs, s, fs->sec) != 0) return -1;
        for (uint32_t off = 0; off + 32 <= fs->sector_size; off += 32) {
            uint8_t first = fs->sec[off];
            if (first == 0x00 || first == 0xE5) {
                out->sector = s;
                out->off = off;
                return 0;
            }
        }
    }
}

static struct vnode *fat_create(struct vnode *dir, const char *name, size_t len, enum vnode_type type)
{
    struct fat *fs = fs_of(dir);
    uint8_t raw[11];
    if (type == VNODE_DEV || !name_to_83(name, len, raw))
        return NULL;
    struct dir_pos pos;
    if (find_free_slot(fs, fn_of(dir), &pos) != 0)
        return NULL;

    struct fat_dirent e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, raw, 11);
    e.attr = type == VNODE_DIR ? ATTR_DIR : 0;
    e.mdate = e.cdate = htole16((uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1));
    if (type == VNODE_DIR) {
        uint32_t c = alloc_cluster(fs);
        if (!c) return NULL;
        e.cluster_hi = htole16((uint16_t)(c >> 16));
        e.cluster_lo = htole16((uint16_t)c);
        /* "." and ".." */
        uint8_t *sec = fs->sec;
        memset(sec, 0, fs->sector_size);
        struct fat_dirent *dot = (void *)sec, *dotdot = (void *)(sec + 32);
        memset(dot->name, ' ', 11); dot->name[0] = '.'; dot->attr = ATTR_DIR;
        dot->cluster_hi = e.cluster_hi; dot->cluster_lo = e.cluster_lo;
        memset(dotdot->name, ' ', 11); dotdot->name[0] = dotdot->name[1] = '.'; dotdot->attr = ATTR_DIR;
        uint32_t pc = fn_of(dir)->fixed_root ? 0 : fn_of(dir)->cluster;
        if (fs->type == 32 && pc == fs->root_cluster) pc = 0;
        dotdot->cluster_hi = htole16((uint16_t)(pc >> 16)); dotdot->cluster_lo = htole16((uint16_t)pc);
        if (write_sector(fs, cluster_sector(fs, c), sec) != 0) return NULL;
    }
    if (read_sector(fs, pos.sector, fs->sec) != 0) return NULL;
    memcpy(fs->sec + pos.off, &e, 32);
    if (write_sector(fs, pos.sector, fs->sec) != 0) return NULL;
    return make_vnode(dir, name, len, &e, &pos);
}

static int empty_cb(struct fat *fs, const struct fat_dirent *e, const char *name, struct dir_pos *pos, void *arg)
{
    (void)fs; (void)e; (void)name; (void)pos; (void)arg;
    return 1;                               /* any entry: not empty */
}

static int fat_unlink(struct vnode *dir, struct vnode *n)
{
    struct fat *fs = fs_of(dir);
    struct fnode *fn = fn_of(n);
    if (n->type == VNODE_DIR && dir_walk(fs, fn, empty_cb, NULL) != 0)
        return -1;
    if (read_sector(fs, fn->dirent_sector, fs->sec) != 0) return -1;
    fs->sec[fn->dirent_off] = 0xE5;
    /* Also drop the long-name entries immediately before it. */
    for (uint32_t off = fn->dirent_off; off >= 32; ) {
        off -= 32;
        if (fs->sec[off + 11] != ATTR_LFN) break;
        fs->sec[off] = 0xE5;
    }
    if (write_sector(fs, fn->dirent_sector, fs->sec) != 0) return -1;
    free_chain(fs, fn->cluster);
    kfree(fn);
    n->priv = NULL;
    return 0;
}

/* ---- file data --------------------------------------------------------------------- */

static long fat_read(struct vnode *n, uint64_t pos, void *buf, size_t len)
{
    struct fat *fs = fs_of(n);
    struct fnode *fn = fn_of(n);
    if (pos >= n->size) return 0;
    if (pos + len > n->size) len = (size_t)(n->size - pos);
    uint8_t *sec = fs->sec;
    size_t done = 0;
    while (done < len) {
        uint64_t p = pos + done;
        uint32_t c = chain_walk(fs, fn->cluster, (uint32_t)(p / fs->cluster_size), 0, NULL);
        if (!c) break;
        uint32_t in_cluster = (uint32_t)(p % fs->cluster_size);
        uint32_t s = cluster_sector(fs, c) + in_cluster / fs->sector_size;
        uint32_t in_sec = in_cluster % fs->sector_size;
        size_t chunk = fs->sector_size - in_sec;
        if (chunk > len - done) chunk = len - done;
        if (read_sector(fs, s, sec) != 0) break;
        memcpy((uint8_t *)buf + done, sec + in_sec, chunk);
        done += chunk;
    }
    return (long)done;
}

static long fat_write(struct vnode *n, uint64_t pos, const void *buf, size_t len)
{
    struct fat *fs = fs_of(n);
    struct fnode *fn = fn_of(n);
    uint8_t *sec = fs->sec;
    size_t done = 0;
    while (done < len) {
        uint64_t p = pos + done;
        uint32_t c = chain_walk(fs, fn->cluster, (uint32_t)(p / fs->cluster_size), 1, &fn->cluster);
        if (!c) break;
        uint32_t in_cluster = (uint32_t)(p % fs->cluster_size);
        uint32_t s = cluster_sector(fs, c) + in_cluster / fs->sector_size;
        uint32_t in_sec = in_cluster % fs->sector_size;
        size_t chunk = fs->sector_size - in_sec;
        if (chunk > len - done) chunk = len - done;
        if (chunk != fs->sector_size && read_sector(fs, s, sec) != 0) break;
        memcpy(sec + in_sec, (const uint8_t *)buf + done, chunk);
        if (write_sector(fs, s, sec) != 0) break;
        done += chunk;
    }
    if (pos + done > n->size)
        n->size = pos + done;
    write_dirent(n);
    return done ? (long)done : (len ? -1 : 0);
}

static int fat_truncate(struct vnode *n, uint64_t size)
{
    if (size != 0) return -1;
    struct fnode *fn = fn_of(n);
    free_chain(fs_of(n), fn->cluster);
    fn->cluster = 0;
    n->size = 0;
    return write_dirent(n);
}

struct readdir_arg { size_t index; struct dirent *out; };

static int readdir_cb(struct fat *fs, const struct fat_dirent *e, const char *name, struct dir_pos *pos, void *arg)
{
    (void)fs;
    struct readdir_arg *ra = arg;
    if (ra->index--)
        return 0;
    size_t l = strlen(name);
    if (l >= VFS_NAME_MAX) l = VFS_NAME_MAX - 1;
    memset(ra->out->name, 0, VFS_NAME_MAX);
    memcpy(ra->out->name, name, l);
    ra->out->type = (e->attr & ATTR_DIR) ? VNODE_DIR : VNODE_FILE;
    ra->out->size = (e->attr & ATTR_DIR) ? 0 : le32toh(e->size);
    uint32_t cl = (uint32_t)le16toh(e->cluster_hi) << 16 | le16toh(e->cluster_lo);
    ra->out->ino = cl ? cl : (pos->sector << 4 | pos->off / 32);
    return 1;
}

static int fat_readdir(struct vnode *dir, size_t index, struct dirent *out)
{
    struct readdir_arg ra = { index, out };
    return dir_walk(fs_of(dir), fn_of(dir), readdir_cb, &ra) == 1 ? 1 : 0;
}

static const struct vnode_ops fat_ops = {
    .lookup   = fat_lookup,
    .create   = fat_create,
    .unlink   = fat_unlink,
    .read     = fat_read,
    .write    = fat_write,
    .truncate = fat_truncate,
    .readdir  = fat_readdir,
};

/* ---- mount --------------------------------------------------------------------------- */

static struct vnode *fat_mount(struct vfs_mount *mnt, struct vnode *devnode, const char *opts)
{
    (void)opts;
    struct blkdev *dev = blkdev_from_vnode(devnode);
    if (!dev) {
        kprintf("fat: not a block device\n");
        return NULL;
    }
    struct fat *fs = kzalloc(sizeof(*fs));
    if (!fs) return NULL;
    fs->dev = dev;
    fs->sector_size = dev->sector_size;
    fs->sec = kmalloc(fs->sector_size);
    fs->fat_cache = kmalloc(fs->sector_size);
    if (!fs->sec || !fs->fat_cache || read_sector(fs, 0, fs->sec) != 0)
        goto bad;

    struct bpb *b = (void *)fs->sec;
    if (le16toh(b->bytes_per_sector) != fs->sector_size || b->sectors_per_cluster == 0 || b->fats == 0 ||
        (fs->sec[510] != 0x55 || fs->sec[511] != 0xAA)) {
        kprintf("fat: %s: no FAT boot sector\n", dev->name);
        goto bad;
    }
    fs->sectors_per_cluster = b->sectors_per_cluster;
    fs->cluster_size = fs->sectors_per_cluster * fs->sector_size;
    fs->fats = b->fats;
    fs->fat_start = le16toh(b->reserved_sectors);
    fs->fat_sectors = b->fat_size16 ? le16toh(b->fat_size16) : le32toh(b->fat_size32);
    uint32_t total = b->total_sectors16 ? le16toh(b->total_sectors16) : le32toh(b->total_sectors32);
    fs->root_sectors = (le16toh(b->root_entries) * 32 + fs->sector_size - 1) / fs->sector_size;
    fs->root_start = fs->fat_start + fs->fats * fs->fat_sectors;
    fs->data_start = fs->root_start + fs->root_sectors;
    fs->clusters = (total - fs->data_start) / fs->sectors_per_cluster;
    fs->type = fs->clusters < 4085 ? 12 : fs->clusters < 65525 ? 16 : 32;
    fs->eoc = fs->type == 12 ? 0xFFF : fs->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    fs->root_cluster = fs->type == 32 ? le32toh(b->root_cluster) : 0;
    fs->next_free = 2;
    char label[12] = "";
    memcpy(label, fs->type == 32 ? b->label : (const char *)(fs->sec + 43), 11);
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';

    struct vnode *root = vnode_alloc("", 0, VNODE_DIR, NULL);
    struct fnode *fn = kzalloc(sizeof(*fn));
    if (!root || !fn) { kfree(root); kfree(fn); goto bad; }
    fn->cluster = fs->root_cluster;
    fn->fixed_root = fs->type != 32;
    root->mnt = mnt;
    root->ops = &fat_ops;
    root->priv = fn;
    root->ino = 1;
    mnt->priv = fs;
    kprintf("fat: mounted %s: FAT%d \"%s\", %u clusters of %u bytes\n",
            dev->name, fs->type, label, fs->clusters, fs->cluster_size);
    return root;
bad:
    kfree(fs->sec);
    kfree(fs->fat_cache);
    kfree(fs);
    return NULL;
}

static void drop_tree(struct vnode *n)
{
    for (struct vnode *c = n->children, *next; c; c = next) {
        next = c->sibling;
        drop_tree(c);
    }
    kfree(n->priv);
    vnode_free(n);
}

static int fat_unmount(struct vfs_mount *mnt)
{
    struct fat *fs = mnt->priv;
    drop_tree(mnt->root);
    kfree(fs->sec);
    kfree(fs->fat_cache);
    kfree(fs);
    return 0;
}

struct fs_type fat_type = { .name = "fat", .mount = fat_mount, .unmount = fat_unmount };
