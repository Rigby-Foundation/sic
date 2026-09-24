/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Modern (1.0+) virtio over PCI: capability discovery, feature negotiation,
 * split virtqueues driven by polling. Enough for the GPU; a NIC or block
 * device would reuse it. */
#pragma once
#include "types.h"
#include "drivers/pci.h"
#include "proc/wait.h"

#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_ID_GPU           16          /* device id 0x1040 + this */

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags, idx; uint16_t ring[]; } __attribute__((packed));
struct virtq_used_elem { uint32_t id, len; } __attribute__((packed));
struct virtq_used  { uint16_t flags, idx; struct virtq_used_elem ring[]; } __attribute__((packed));
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

struct virtq {
    uint16_t size, index;
    uint16_t free_head, last_used;
    struct virtq_desc  *desc;           /* all three in one physically contiguous block */
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t phys;
    volatile uint16_t *notify;          /* this queue's doorbell */
};

struct virtio_dev {
    const struct pci_dev *pci;
    volatile uint8_t *common, *notify_base, *isr, *device_cfg;
    uint32_t notify_mult;
};

/* Finds the device, maps its regions, resets it, negotiates `features`
 * (VERSION_1 is added). Returns 0 or -1. */
int  virtio_init(struct virtio_dev *d, const struct pci_dev *pci, uint64_t features);
int  virtio_queue_setup(struct virtio_dev *d, struct virtq *q, uint16_t index);
void virtio_driver_ok(struct virtio_dev *d);
uint32_t virtio_cfg_read32(struct virtio_dev *d, uint32_t off);
void     virtio_cfg_write32(struct virtio_dev *d, uint32_t off, uint32_t v);

/* One request: an out buffer the device reads and an in buffer it writes,
 * both in the direct map (pmm pages through P2V; not the heap, not the
 * kernel image). Waits (polling) for completion; returns the number of
 * bytes the device wrote, or -1 on timeout. */
long virtio_request(struct virtq *q, const void *out, size_t out_len, void *in, size_t in_len);
