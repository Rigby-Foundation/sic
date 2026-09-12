/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* User-visible ABI types and constants for sic (what musl's x86_64 bits/ expect). */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif
#include "abi/syscall_nr.h"

/* ELF e_ident[EI_OSABI] for sic executables. The kernel only runs ELFs
 * stamped with this; binaries for other systems fail with ENOEXEC up front
 * instead of dying on their first system call. */
#define ELFOSABI_SIC 0x53   /* 'S' */
#define EI_OSABI     7
#define EI_ABIVERSION 8

/* errno (returned as -errno from system calls) */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EMFILE      24
#define ENOTTY      25
#define ENOSPC      28
#define ESPIPE      29
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define EBUSY       16
#define ENODEV      19
#define ENOEXEC      8
#define E2BIG        7

#define AT_FDCWD      (-100)
#define AT_EMPTY_PATH 0x1000

/* open flags (O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND are in vfs.h with the same values) */
#define O_ACCMODE   03
#define O_EXCL      0200
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

/* fcntl */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

/* ioctl */
#define TCGETS     0x5401
#define TIOCGWINSZ 0x5413

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

/* stat: musl arch/x86_64/bits/stat.h, 144 bytes */
struct abi_timespec { int64_t tv_sec; int64_t tv_nsec; };

struct abi_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    struct abi_timespec st_atim, st_mtim, st_ctim;
    int64_t  __unused[3];
};

#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000

/* getdents64 */
struct abi_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};
#define DT_DIR 4
#define DT_REG 8
#define DT_CHR 2

struct abi_iovec { uint64_t iov_base; uint64_t iov_len; };

struct abi_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

/* mmap */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* arch_prctl */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* wait4 */
#define WNOHANG 1
#define W_EXITCODE(code) (((code) & 0xFF) << 8)
#define W_SIGNALED(sig)  ((sig) & 0x7F)
#define SIGSEGV 11
#define SIGILL   4
#define SIGFPE   8
#define SIGBUS   7

/* clocks */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* auxv */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
