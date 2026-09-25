/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/shmem: shared memory between processes. Open it, SHM_IOC_CREATE a
 * segment (or SHM_IOC_ATTACH one by the key another process passed you),
 * mmap the file with offset 0. The segment lives while any open file
 * refers to it; keep the file open while the mapping is in use. */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

struct shm_segment {
    uint64_t size;              /* CREATE: in, rounded up to pages; INFO: out */
    uint32_t key;               /* CREATE/INFO: out; ATTACH: in */
    uint32_t pad;
};

#define SHM_IOC_CREATE 0x53000  /* struct shm_segment * */
#define SHM_IOC_ATTACH 0x53001  /* struct shm_segment * (key) */
#define SHM_IOC_INFO   0x53002  /* struct shm_segment * */
