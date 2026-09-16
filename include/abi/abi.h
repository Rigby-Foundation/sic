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
#include "abi/fb.h"

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
#define EPIPE       32
#define ETIMEDOUT  110
#define ENOTSUP     95
#define EWOULDBLOCK EAGAIN
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
#define BLKRRPART  0x125F
#define BLKGETSIZE64 0x80081272
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

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
#define S_IFBLK 0060000
#define S_IFSOCK 0140000
#define S_IFIFO 0010000

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

/* ---- networking (CONFIG_NET) ---------------------------------------------------- */
/* errno values musl's socket code expects (Linux numbering, as the rest of errno) */
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETUNREACH    101
#define ECONNABORTED   103
#define ECONNRESET     104
#define ENOBUFS        105
#define EISCONN        106
#define ENOTCONN       107
#define ECONNREFUSED   111
#define EHOSTUNREACH   113
#define EALREADY       114
#define EINPROGRESS    115

#define O_NONBLOCK  04000

/* poll */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
struct abi_pollfd { int32_t fd; int16_t events; int16_t revents; };

/* sockets: musl's generic bits/socket.h layouts */
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_TYPE_MASK 0xF
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_ERROR     4
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SOL_TCP      6
#define TCP_NODELAY  1
#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct abi_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;              /* network byte order */
    uint32_t sin_addr;              /* network byte order */
    uint8_t  sin_zero[8];
};

struct abi_msghdr {                 /* musl x86_64 */
    uint64_t msg_name;
    uint32_t msg_namelen, __pad1;
    uint64_t msg_iov;
    int32_t  msg_iovlen, __pad2;
    uint64_t msg_control;
    uint32_t msg_controllen, __pad3;
    int32_t  msg_flags;
};

/* interface configuration: ioctl on any socket with a Linux-shaped ifreq */
#define IFNAMSIZ 16
struct abi_ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct abi_sockaddr_in ifr_addr;    /* SIOC[GS]IF{ADDR,NETMASK,GATEWAY,BRDADDR} */
        uint8_t ifr_hwaddr[16];             /* SIOCGIFHWADDR: sa_family(2) + MAC(6) */
        int16_t ifr_flags;                  /* SIOC[GS]IFFLAGS */
        int32_t ifr_ifindex;                /* SIOCGIFINDEX, SIOCGIFNAME */
        int32_t ifr_mtu;                    /* SIOCGIFMTU */
        uint8_t ifr_pad[24];
    };
};
#define SIOCGIFNAME    0x8910
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFGATEWAY 0x89F0       /* sic: default gateway of this interface */
#define SIOCSIFGATEWAY 0x89F1
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
