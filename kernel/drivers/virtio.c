/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Modern virtio-pci transport: the vendor capabilities point at the common,
 * notify, ISR and device-specific windows inside the BARs (registers are
 * little-endian); virtqueues are the split layout in guest memory. */
#include "drivers/virtio.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "endian.h"
#include "asm/timer.h"
#include "asm/irqflags.h"
#include "proc/wait.h"
#include "proc/sched.h"
#include "string.h"
#include "printf.h"

/* common configuration layout */
#define C_DEV_FEAT_SEL   0x00
#define C_DEV_FEAT       0x04
#define C_DRV_FEAT_SEL   0x08
#define C_DRV_FEAT       0x0C
#define C_NUM_QUEUES     0x12
#define C_STATUS         0x14
#define C_Q_SELECT       0x16
#define C_Q_SIZE         0x18
#define C_Q_ENABLE       0x1C
#define C_Q_NOTIFY_OFF   0x1E
#define C_Q_DESC         0x20
#define C_Q_DRIVER       0x28
#define C_Q_DEVICE       0x30

#define STATUS_ACK       1
#define STATUS_DRIVER    2
#define STATUS_DRIVER_OK 4
#define STATUS_FEAT_OK   8
#define VIRTIO_F_VERSION_1 (1ULL << 32)

static uint8_t  c8(struct virtio_dev *d, uint32_t o)  { return mmio_read8(d->common + o); }
static uint16_t c16(struct virtio_dev *d, uint32_t o) { return mmio_read16(d->common + o); }
static uint32_t c32(struct virtio_dev *d, uint32_t o) { return mmio_read32(d->common + o); }
static void w8(struct virtio_dev *d, uint32_t o, uint8_t v)   { mmio_write8(d->common + o, v); }
static void w16(struct virtio_dev *d, uint32_t o, uint16_t v) { mmio_write16(d->common + o, v); }
static void w32(struct virtio_dev *d, uint32_t o, uint32_t v) { mmio_write32(d->common + o, v); }
static void w64(struct virtio_dev *d, uint32_t o, uint64_t v) { mmio_write32(d->common + o, (uint32_t)v); mmio_write32(d->common + o + 4, (uint32_t)(v >> 32)); }

uint32_t virtio_cfg_read32(struct virtio_dev *d, uint32_t off) { return mmio_read32(d->device_cfg + off); }
void     virtio_cfg_write32(struct virtio_dev *d, uint32_t off, uint32_t v) { mmio_write32(d->device_cfg + off, v); }

/* Map the window a vendor capability describes: BAR + offset, `len` bytes. */
static volatile uint8_t *map_cap(const struct pci_dev *pci, uint8_t bar, uint32_t off, uint32_t len)
{
    if (bar > 5 || pci->bar_is_io[bar] || !pci->bar[bar]) return NULL;
    return vmm_map_mmio(pci->bar[bar] + off, len ? len : 0x1000);
}

int virtio_init(struct virtio_dev *d, const struct pci_dev *pci, uint64_t features)
{
    memset(d, 0, sizeof *d);
    d->pci = pci;
    uint32_t status = pci_read32(pci->bus, pci->slot, pci->func, 4);
    if (!(status & (1u << 20))) return -1;                      /* no capability list */
    uint8_t ptr = (uint8_t)(pci_read32(pci->bus, pci->slot, pci->func, 0x34) & 0xFC);
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint32_t c0 = pci_read32(pci->bus, pci->slot, pci->func, ptr);
        uint8_t id = (uint8_t)c0, next = (uint8_t)(c0 >> 8), type = (uint8_t)(c0 >> 24);
        if (id == 0x09) {
            uint8_t bar = (uint8_t)pci_read32(pci->bus, pci->slot, pci->func, ptr + 4);
            uint32_t off = pci_read32(pci->bus, pci->slot, pci->func, ptr + 8);
            uint32_t len = pci_read32(pci->bus, pci->slot, pci->func, ptr + 12);
            switch (type) {
            case 1: if (!d->common) d->common = map_cap(pci, bar, off, len); break;
            case 2: if (!d->notify_base) { d->notify_base = map_cap(pci, bar, off, len); d->notify_mult = pci_read32(pci->bus, pci->slot, pci->func, ptr + 16); } break;
            case 3: if (!d->isr) d->isr = map_cap(pci, bar, off, len); break;
            case 4: if (!d->device_cfg) d->device_cfg = map_cap(pci, bar, off, len); break;
            }
        }
        ptr = next & 0xFC;
    }
    if (!d->common || !d->notify_base) {
        kprintf("virtio: %02x:%02x.%u: no modern capabilities\n", pci->bus, pci->slot, pci->func);
        return -1;
    }
    pci_enable_busmaster(pci);

    w8(d, C_STATUS, 0);                                         /* reset */
    for (int i = 0; i < 1000 && c8(d, C_STATUS) != 0; i++) ;
    w8(d, C_STATUS, STATUS_ACK);
    w8(d, C_STATUS, STATUS_ACK | STATUS_DRIVER);
    features |= VIRTIO_F_VERSION_1;
    w32(d, C_DEV_FEAT_SEL, 0); uint32_t f0 = c32(d, C_DEV_FEAT);
    w32(d, C_DEV_FEAT_SEL, 1); uint32_t f1 = c32(d, C_DEV_FEAT);
    uint64_t offered = (uint64_t)f1 << 32 | f0;
    if (!(offered & VIRTIO_F_VERSION_1)) { kprintf("virtio: legacy-only device\n"); return -1; }
    features &= offered;
    w32(d, C_DRV_FEAT_SEL, 0); w32(d, C_DRV_FEAT, (uint32_t)features);
    w32(d, C_DRV_FEAT_SEL, 1); w32(d, C_DRV_FEAT, (uint32_t)(features >> 32));
    w8(d, C_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEAT_OK);
    if (!(c8(d, C_STATUS) & STATUS_FEAT_OK)) { kprintf("virtio: features rejected\n"); return -1; }
    return 0;
}

int virtio_queue_setup(struct virtio_dev *d, struct virtq *q, uint16_t index)
{
    memset(q, 0, sizeof *q);
    w16(d, C_Q_SELECT, index);
    uint16_t size = c16(d, C_Q_SIZE);
    if (size == 0) return -1;
    if (size > 128) size = 128;
    q->size = size; q->index = index;
    /* desc (16 * size) + avail (6 + 2 * size) in the first pages, used (6 + 8 * size) page-aligned after */
    size_t desc_bytes = 16u * size, avail_bytes = 6u + 2u * size, used_bytes = 6u + 8u * size;
    size_t used_off = PAGE_ALIGN_UP(desc_bytes + avail_bytes);
    size_t pages = PAGE_ALIGN_UP(used_off + used_bytes) / PAGE_SIZE;
    q->phys = pmm_alloc_pages(pages);
    if (!q->phys) return -1;
    uint8_t *v = P2V(q->phys);
    memset(v, 0, pages * PAGE_SIZE);
    q->desc = (void *)v;
    q->avail = (void *)(v + desc_bytes);
    q->used = (void *)(v + used_off);
    for (uint16_t i = 0; i < size; i++) q->desc[i].next = htole16((uint16_t)(i + 1));
    q->free_head = 0;
    w16(d, C_Q_SIZE, size);
    w64(d, C_Q_DESC, q->phys);
    w64(d, C_Q_DRIVER, q->phys + desc_bytes);
    w64(d, C_Q_DEVICE, q->phys + used_off);
    q->notify = (volatile uint16_t *)(d->notify_base + (uint32_t)c16(d, C_Q_NOTIFY_OFF) * d->notify_mult);
    w16(d, C_Q_ENABLE, 1);
    return 0;
}

void virtio_driver_ok(struct virtio_dev *d)
{
    w8(d, C_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEAT_OK | STATUS_DRIVER_OK);
}

unsigned long virtio_irqs;

long virtio_request(struct virtq *q, const void *out, size_t out_len, void *in, size_t in_len)
{
    uint16_t d0 = q->free_head, d1 = le16toh(q->desc[d0].next);
    q->free_head = le16toh(q->desc[d1].next);
    q->desc[d0].addr = htole64(V2P(out));
    q->desc[d0].len = htole32((uint32_t)out_len);
    q->desc[d0].flags = htole16(VIRTQ_DESC_F_NEXT);
    q->desc[d0].next = htole16(d1);
    q->desc[d1].addr = htole64(V2P(in));
    q->desc[d1].len = htole32((uint32_t)in_len);
    q->desc[d1].flags = htole16(VIRTQ_DESC_F_WRITE);
    q->desc[d1].next = 0;
    uint16_t idx = le16toh(q->avail->idx);
    q->avail->ring[idx % q->size] = htole16(d0);
    dma_wmb();
    q->avail->idx = htole16((uint16_t)(idx + 1));
    dma_wmb();
    mmio_write16(q->notify, q->index);

    uint64_t deadline = timer_ticks() + 2000, renotify = timer_ticks() + 1;
    long got = -1;
    for (;;) {
        dma_rmb();
        if (le16toh(q->used->idx) != q->last_used) {
            struct virtq_used_elem *e = &q->used->ring[q->last_used % q->size];
            q->last_used++;
            if (le32toh(e->id) == d0) { got = (long)le32toh(e->len); break; }
            continue;                                       /* someone else's; keep going */
        }
        if (timer_ticks() > deadline) break;
        /* a fenced request is answered when the host next looks at its
         * fences: on a notify, or on its own timer (10 ms in QEMU) */
        if (timer_ticks() >= renotify) { mmio_write16(q->notify, q->index); renotify = timer_ticks() + 1; }
        cpu_relax();
    }
    /* give the two descriptors back */
    q->desc[d1].next = htole16(q->free_head);
    q->desc[d0].next = htole16(d1);
    q->free_head = d0;
    return got;
}
