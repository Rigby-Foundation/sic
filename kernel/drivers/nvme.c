/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* NVMe: a minimal polled driver. One admin and one I/O queue pair per
 * controller, one command in flight, transfers of up to one page. */
#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "fs/blkdev.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "arch/x86_64/timer.h"

#define REG_CAP   0x00
#define REG_VS    0x08
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30

#define CC_EN     (1 << 0)
#define CC_IOSQES (6 << 16)
#define CC_IOCQES (4 << 20)
#define CSTS_RDY  (1 << 0)
#define CSTS_CFS  (1 << 1)

#define QUEUE_DEPTH 32

#define OP_ADMIN_CREATE_SQ 0x01
#define OP_ADMIN_CREATE_CQ 0x05
#define OP_ADMIN_IDENTIFY  0x06
#define OP_IO_WRITE        0x01
#define OP_IO_READ         0x02

struct sqe {
    uint8_t  opcode, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

struct cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;        /* bit 0 = phase */
} __attribute__((packed));

struct queue {
    struct sqe *sq;         /* virtual (HHDM) */
    struct cqe *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t sq_tail, cq_head;
    uint8_t  phase;
    uint16_t id;
};

struct nvme {
    volatile uint8_t *regs;
    uint32_t dstrd;
    struct queue admin, io;
    uint16_t cid;
    uint64_t buf_phys;      /* bounce page for I/O */
    spinlock_t lock;
    struct blkdev bdev;
    uint32_t lba_shift;
};

static inline uint32_t rd32(struct nvme *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline uint64_t rd64(struct nvme *c, uint32_t r) { return *(volatile uint64_t *)(c->regs + r); }
static inline void wr32(struct nvme *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }
static inline void wr64(struct nvme *c, uint32_t r, uint64_t v) { *(volatile uint64_t *)(c->regs + r) = v; }

static void doorbell_sq(struct nvme *c, struct queue *q) { wr32(c, 0x1000 + (2 * q->id) * (4 << c->dstrd), q->sq_tail); }
static void doorbell_cq(struct nvme *c, struct queue *q) { wr32(c, 0x1000 + (2 * q->id + 1) * (4 << c->dstrd), q->cq_head); }

static int queue_alloc(struct queue *q, uint16_t id)
{
    q->sq_phys = pmm_alloc_page();
    q->cq_phys = pmm_alloc_page();
    if (!q->sq_phys || !q->cq_phys)
        return -1;
    q->sq = P2V(q->sq_phys);
    q->cq = P2V(q->cq_phys);
    memset(q->sq, 0, PAGE_SIZE);
    memset(q->cq, 0, PAGE_SIZE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return 0;
}

/* Submit one command and spin for its completion. Returns the status field
 * (0 = success) or -1 on timeout; *result gets DW0 of the completion. */
static int submit(struct nvme *c, struct queue *q, struct sqe *cmd, uint32_t *result)
{
    cmd->cid = c->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % QUEUE_DEPTH;
    doorbell_sq(c, q);

    struct cqe *e = &q->cq[q->cq_head];
    uint64_t deadline = timer_ticks() + 2000;
    while ((e->status & 1) != q->phase) {
        __asm__ volatile("pause");
        if (timer_ticks() > deadline)
            return -1;
    }
    int status = e->status >> 1;
    if (result)
        *result = e->result;
    q->cq_head = (q->cq_head + 1) % QUEUE_DEPTH;
    if (q->cq_head == 0)
        q->phase ^= 1;
    doorbell_cq(c, q);
    return status;
}

static int nvme_rw(struct blkdev *d, uint64_t lba, uint32_t count, void *buf, int write)
{
    struct nvme *c = d->priv;
    uint32_t per_page = PAGE_SIZE >> c->lba_shift;
    uint8_t *p = buf;
    int rc = 0;

    spin_lock(&c->lock);
    while (count && rc == 0) {
        uint32_t n = count < per_page ? count : per_page;
        size_t bytes = (size_t)n << c->lba_shift;
        if (write)
            memcpy(P2V(c->buf_phys), p, bytes);
        struct sqe cmd = {
            .opcode = write ? OP_IO_WRITE : OP_IO_READ,
            .nsid = 1,
            .prp1 = c->buf_phys,
            .cdw10 = (uint32_t)lba,
            .cdw11 = (uint32_t)(lba >> 32),
            .cdw12 = n - 1,
        };
        int st = submit(c, &c->io, &cmd, NULL);
        if (st != 0) {
            kprintf("nvme: %s lba %lu failed, status %x\n", write ? "write" : "read", lba, st);
            rc = -1;
            break;
        }
        if (!write)
            memcpy(p, P2V(c->buf_phys), bytes);
        p += bytes;
        lba += n;
        count -= n;
    }
    spin_unlock(&c->lock);
    return rc;
}

static int nvme_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)        { return nvme_rw(d, lba, count, buf, 0); }
static int nvme_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf) { return nvme_rw(d, lba, count, (void *)buf, 1); }

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid, void *out)
{
    struct sqe cmd = { .opcode = OP_ADMIN_IDENTIFY, .nsid = nsid, .prp1 = c->buf_phys, .cdw10 = cns };
    int st = submit(c, &c->admin, &cmd, NULL);
    if (st == 0)
        memcpy(out, P2V(c->buf_phys), PAGE_SIZE);
    return st;
}

static int nvme_probe(const struct pci_dev *pd, int index)
{
    struct nvme *c = kzalloc(sizeof(*c));
    if (!c)
        return -1;
    pci_enable_busmaster(pd);
    c->regs = vmm_map_mmio(pd->bar[0], 0x4000);

    uint64_t cap = rd64(c, REG_CAP);
    c->dstrd = (cap >> 32) & 0xF;
    uint32_t timeout_ms = ((cap >> 24) & 0xFF) * 500;

    /* Reset. */
    wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
    uint64_t deadline = timer_ticks() + timeout_ms + 100;
    while (rd32(c, REG_CSTS) & CSTS_RDY)
        if (timer_ticks() > deadline) { kprintf("nvme: reset timeout\n"); return -1; }

    if (queue_alloc(&c->admin, 0) != 0 || queue_alloc(&c->io, 1) != 0)
        return -1;
    c->buf_phys = pmm_alloc_page();

    wr32(c, REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    wr32(c, REG_CC, CC_IOSQES | CC_IOCQES | CC_EN);   /* MPS = 0 (4 KiB), CSS = NVM */
    deadline = timer_ticks() + timeout_ms + 100;
    while (!(rd32(c, REG_CSTS) & CSTS_RDY)) {
        if (rd32(c, REG_CSTS) & CSTS_CFS) { kprintf("nvme: controller fatal\n"); return -1; }
        if (timer_ticks() > deadline) { kprintf("nvme: enable timeout\n"); return -1; }
    }

    uint8_t *id = kmalloc(PAGE_SIZE);
    if (identify(c, 1, 0, id) != 0) { kprintf("nvme: identify controller failed\n"); return -1; }
    char model[41];
    memcpy(model, id + 24, 40);
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;

    /* One I/O queue pair. */
    struct sqe cq = { .opcode = OP_ADMIN_CREATE_CQ, .prp1 = c->io.cq_phys,
                      .cdw10 = ((QUEUE_DEPTH - 1) << 16) | 1, .cdw11 = 1 /* physically contiguous, no IRQ */ };
    if (submit(c, &c->admin, &cq, NULL) != 0) { kprintf("nvme: create cq failed\n"); return -1; }
    struct sqe sq = { .opcode = OP_ADMIN_CREATE_SQ, .prp1 = c->io.sq_phys,
                      .cdw10 = ((QUEUE_DEPTH - 1) << 16) | 1, .cdw11 = (1 << 16) | 1 };
    if (submit(c, &c->admin, &sq, NULL) != 0) { kprintf("nvme: create sq failed\n"); return -1; }

    /* Namespace 1. */
    if (identify(c, 0, 1, id) != 0) { kprintf("nvme: identify namespace failed\n"); return -1; }
    uint64_t nsze;
    memcpy(&nsze, id, 8);
    uint8_t flbas = id[26] & 0xF;
    uint8_t lbads = id[128 + flbas * 4 + 2];
    c->lba_shift = lbads;
    kfree(id);

    memcpy(c->bdev.name, "nvme0n1", 8);
    c->bdev.name[4] = (char)('0' + index);
    c->bdev.sector_size = 1u << lbads;
    c->bdev.sectors = nsze;
    c->bdev.read = nvme_read;
    c->bdev.write = nvme_write;
    c->bdev.priv = c;
    kprintf("nvme: %s (%s), version %x.%x\n", c->bdev.name, model, rd32(c, REG_VS) >> 16, (rd32(c, REG_VS) >> 8) & 0xFF);
    blkdev_register(&c->bdev);
    return 0;
}

void nvme_init(void)
{
    const struct pci_dev *pd;
    for (int i = 0; (pd = pci_find_class(0x01, 0x08, i)); i++)
        if (pd->prog_if == 0x02)
            nvme_probe(pd, i);
}
