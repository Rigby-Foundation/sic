/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * zaefs on-disk format (version 1). Shared by the kernel driver and the
 * user-space tools (mkfs.zaefs); everything is little-endian.
 *
 *   block 0            superblock
 *   bbitmap_start..    block bitmap   (1 bit per block, 1 = used)
 *   ibitmap_start..    inode bitmap   (1 bit per inode)
 *   itable_start..     inode table    (ZAEFS_INODE_SIZE bytes each, inode 0 unused)
 *   data_start..       data blocks
 *
 * Files: 12 direct + 1 indirect + 1 double-indirect 32-bit block pointers.
 * Directories: a sequence of zaefs_dirent records per block, each padded to
 * 8 bytes; rec_len of the last record reaches the end of the block; ino 0
 * marks a free record. Directories store neither "." nor "..".
 */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

#define ZAEFS_MAGIC        0x0100736665617ALL   /* "zaefs\0\0\1" as a little-endian u64 */
#define ZAEFS_VERSION      1
#define ZAEFS_BLOCK_SIZE   4096
#define ZAEFS_INODE_SIZE   128
#define ZAEFS_NDIRECT      12
#define ZAEFS_ROOT_INO     1
#define ZAEFS_NAME_MAX     255
#define ZAEFS_PTRS_PER_BLOCK (ZAEFS_BLOCK_SIZE / 4)

struct zaefs_superblock {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t bbitmap_start, bbitmap_blocks;
    uint64_t ibitmap_start, ibitmap_blocks;
    uint64_t itable_start,  itable_blocks;
    uint64_t data_start;
    uint64_t free_blocks;
    uint64_t free_inodes;
    uint64_t root_ino;
    char     label[32];
    uint8_t  pad[ZAEFS_BLOCK_SIZE - 8 - 4 - 4 - 8 * 12 - 32];
};

#define ZAEFS_TYPE_FILE 1
#define ZAEFS_TYPE_DIR  2

struct zaefs_inode {
    uint16_t type;
    uint16_t links;
    uint32_t mode;
    uint64_t size;
    uint64_t ctime;
    uint64_t mtime;
    uint32_t nblocks;                   /* data + index blocks owned */
    uint32_t direct[ZAEFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
    uint8_t  pad[ZAEFS_INODE_SIZE - 2 - 2 - 4 - 8 * 3 - 4 - 4 * ZAEFS_NDIRECT - 4 - 4];
};

struct zaefs_dirent {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  type;
    char     name[];
};

#define ZAEFS_DIRENT_SIZE(name_len) (((8 + (name_len)) + 7) & ~7u)

/* Byte order: the disk is little-endian. On a big-endian CPU the kernel and
 * the tools swap the superblock and inodes at the disk boundary with these
 * (each swap is its own inverse); dirents and block pointers are handled
 * field by field where they are read. */
#define ZAEFS_HOST_IS_BE (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

static inline uint16_t zaefs_bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t zaefs_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t zaefs_bswap64(uint64_t v) { return __builtin_bswap64(v); }

static inline void zaefs_sb_swap(struct zaefs_superblock *sb)
{
    sb->magic = zaefs_bswap64(sb->magic);
    sb->version = zaefs_bswap32(sb->version);
    sb->block_size = zaefs_bswap32(sb->block_size);
    uint64_t *f = &sb->total_blocks;
    for (int i = 0; i < 12; i++) f[i] = zaefs_bswap64(f[i]);      /* total_blocks .. root_ino */
}

static inline void zaefs_inode_swap(struct zaefs_inode *di)
{
    di->type = zaefs_bswap16(di->type);
    di->links = zaefs_bswap16(di->links);
    di->mode = zaefs_bswap32(di->mode);
    di->size = zaefs_bswap64(di->size);
    di->ctime = zaefs_bswap64(di->ctime);
    di->mtime = zaefs_bswap64(di->mtime);
    di->nblocks = zaefs_bswap32(di->nblocks);
    for (int i = 0; i < ZAEFS_NDIRECT; i++) di->direct[i] = zaefs_bswap32(di->direct[i]);
    di->indirect = zaefs_bswap32(di->indirect);
    di->dindirect = zaefs_bswap32(di->dindirect);
}

/* Host <-> disk for the superblock and an inode: a no-op on little-endian CPUs. */
#define ZAEFS_SB_SWAP(sb)    do { if (ZAEFS_HOST_IS_BE) zaefs_sb_swap(sb); } while (0)
#define ZAEFS_INODE_SWAP(di) do { if (ZAEFS_HOST_IS_BE) zaefs_inode_swap(di); } while (0)
