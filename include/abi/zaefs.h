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
