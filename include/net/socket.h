/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Socket system calls (kernel/net/socket.c), dispatched from proc/syscall.c. */
#pragma once
#include "types.h"

long sys_socket(long domain, long type, long proto);
long sys_bind(long fd, uint64_t uaddr, uint64_t ulen);
long sys_listen(long fd, long backlog);
long sys_connect(long fd, uint64_t uaddr, uint64_t ulen);
long sys_accept4(long fd, uint64_t uaddr, uint64_t ulenp, long flags);
long sys_shutdown(long fd, long how);
long sys_getsockname(long fd, uint64_t uaddr, uint64_t ulenp);
long sys_getpeername(long fd, uint64_t uaddr, uint64_t ulenp);
long sys_setsockopt(long fd, long level, long name, uint64_t uval, uint64_t len);
long sys_getsockopt(long fd, long level, long name, uint64_t uval, uint64_t ulenp);
long sys_sendto(long fd, uint64_t ubuf, uint64_t len, long flags, uint64_t uaddr, uint64_t ulen);
long sys_recvfrom(long fd, uint64_t ubuf, uint64_t len, long flags, uint64_t uaddr, uint64_t ulenp);
long sys_sendmsg(long fd, uint64_t umsg, long flags);
long sys_recvmsg(long fd, uint64_t umsg, long flags);
