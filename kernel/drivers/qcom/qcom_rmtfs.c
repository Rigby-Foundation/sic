/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The modem's remote file system (rmtfs): the modem keeps its EFS (its
 * NV items: IMEI, calibration, network settings) on the phone's modemst1,
 * modemst2, fsg and fsc partitions, which only the apps processor can
 * reach. It asks over QMI (service 14 on QRTR) to open them, for a shared
 * buffer, and for sectors moved between them and the buffer, as
 * linux-msm/rmtfs serves it. Until it is served, the modem's start stalls
 * and its watchdog fires.
 * Read-only, as `rmtfs -r`: each partition is read into RAM once, and what
 * the modem writes stays in that copy; nothing goes back to the flash. */
#include "asm/memlayout.h"
#include "asm/qcom_ipc.h"
#include "asm/qcom_scm.h"
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

#define RMTFS_SERVICE       14
#define RMTFS_VERSION       1
#define RMTFS_INSTANCE      0
#define RMTFS_PORT          0x100
#define RMTFS_MEM_SIZE      0x300000            /* the DT's qcom,rmtfs_sharedmem */
#define SECTOR              512

enum { QMI_REQUEST = 0, QMI_RESPONSE = 2 };
enum { RMTFS_OPEN = 1, RMTFS_CLOSE = 2, RMTFS_RW_IOVEC = 3, RMTFS_ALLOC_BUFF = 4, RMTFS_GET_DEV_ERROR = 5 };

static uint64_t mem_phys;
static uint8_t *mem;                            /* the shared buffer, write-combining */

static const struct { const char *path, *label; } table[] = {
    { "/boot/modem_fs1", "modemst1" }, { "/boot/modem_fs2", "modemst2" },
    { "/boot/modem_fsc", "fsc" }, { "/boot/modem_fsg", "fsg" },
};

#define MAX_CALLERS 8
static struct { int open; const char *label; uint8_t *shadow; size_t size; } callers[MAX_CALLERS];
static uint32_t n_open, n_read, n_write, n_bad;

/* ---- QMI ---------------------------------------------------------------------------------------- */

struct tlv { uint8_t type; uint16_t len; const uint8_t *v; };

static int find_tlv(const uint8_t *msg, size_t len, uint8_t type, struct tlv *t)
{
    for (size_t o = 0; o + 3 <= len; ) {
        uint16_t l = (uint16_t)(msg[o + 1] | msg[o + 2] << 8);
        if (o + 3 + l > len) return -1;
        if (msg[o] == type) { t->type = type; t->len = l; t->v = msg + o + 3; return 0; }
        o += 3 + (size_t)l;
    }
    return -1;
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct reply { uint8_t buf[64]; size_t len; };

static void reply_start(struct reply *r, uint16_t msg, uint16_t txn, uint16_t result, uint16_t error)
{
    r->buf[0] = QMI_RESPONSE;
    r->buf[1] = (uint8_t)txn; r->buf[2] = (uint8_t)(txn >> 8);
    r->buf[3] = (uint8_t)msg; r->buf[4] = (uint8_t)(msg >> 8);
    r->len = 7;
    uint8_t res[4] = { (uint8_t)result, (uint8_t)(result >> 8), (uint8_t)error, (uint8_t)(error >> 8) };
    r->buf[r->len++] = 2; r->buf[r->len++] = 4; r->buf[r->len++] = 0;     /* TLV 2: the result */
    memcpy(r->buf + r->len, res, 4); r->len += 4;
}

static void reply_tlv(struct reply *r, uint8_t type, const void *v, uint16_t len)
{
    r->buf[r->len++] = type; r->buf[r->len++] = (uint8_t)len; r->buf[r->len++] = (uint8_t)(len >> 8);
    memcpy(r->buf + r->len, v, len); r->len += len;
}

static void reply_send(struct reply *r, uint32_t node, uint32_t port)
{
    uint16_t body = (uint16_t)(r->len - 7);
    r->buf[5] = (uint8_t)body; r->buf[6] = (uint8_t)(body >> 8);
    qrtr_sendto(RMTFS_PORT, node, port, r->buf, r->len);
}

/* ---- the requests ----------------------------------------------------------------------------------- */

static int do_open(const struct tlv *path)
{
    char p[64] = { 0 };
    memcpy(p, path->v, path->len < sizeof p - 1 ? path->len : sizeof p - 1);
    const char *label = NULL;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) if (strcmp(p, table[i].path) == 0) label = table[i].label;
    if (!label) { kprintf("rmtfs: open %s: not a partition it knows\n", p); return -1; }
    for (int i = 0; i < MAX_CALLERS; i++) if (callers[i].open && callers[i].label == label) return i;
    int id = -1;
    for (int i = 0; i < MAX_CALLERS && id < 0; i++) if (!callers[i].open) id = i;
    if (id < 0) return -1;
    char dev[48];
    ksnprintf(dev, sizeof dev, "/dev/by-name/%s", label);
    struct vnode *v = vfs_lookup(vfs_root(), dev);
    struct blkdev *b = v ? blkdev_from_vnode(v) : NULL;
    if (!b) { kprintf("rmtfs: open %s: no %s\n", p, dev); return -1; }
    if (!callers[id].shadow) {
        size_t size = (size_t)(b->sectors * b->sector_size);
        uint8_t *s = kmalloc(size);
        if (!s || blkdev_read_bytes(b, 0, s, size) != (long)size) { if (s) kfree(s); kprintf("rmtfs: %s does not read\n", dev); return -1; }
        callers[id].shadow = s; callers[id].size = size;
    }
    callers[id].open = 1; callers[id].label = label;
    n_open++;
    kprintf("rmtfs: %s -> %s (%lu KiB), caller %d\n", p, label, (unsigned long)(callers[id].size >> 10), id);
    return id;
}

/* The sectors the modem names, between our copy of the partition and the
 * shared buffer (its physical addresses). */
static int do_iovec(int id, int write, const struct tlv *iov)
{
    if (id < 0 || id >= MAX_CALLERS || !callers[id].open || iov->len < 1) return -1;
    unsigned n = iov->v[0];
    if (iov->len < 1 + 12u * n) return -1;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *e = iov->v + 1 + 12 * i;
        uint64_t sector = get32(e), phys = get32(e + 4), count = get32(e + 8);
        uint64_t off = sector * SECTOR, bytes = count * SECTOR;
        if (phys < mem_phys || phys + bytes > mem_phys + RMTFS_MEM_SIZE || off + bytes > callers[id].size) {
            kprintf("rmtfs: %s %s sectors %lu+%lu at %lx: out of range\n", callers[id].label, write ? "write" : "read",
                    (unsigned long)sector, (unsigned long)count, (unsigned long)phys);
            return -1;
        }
        if (write) memcpy(callers[id].shadow + off, mem + (phys - mem_phys), bytes);
        else memcpy(mem + (phys - mem_phys), callers[id].shadow + off, bytes);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (write) n_write++; else n_read++;
    return 0;
}

static void rmtfs_rx(uint32_t node, uint32_t port, const uint8_t *msg, size_t len)
{
    if (len < 7 || msg[0] != QMI_REQUEST) return;
    uint16_t txn = (uint16_t)(msg[1] | msg[2] << 8), id = (uint16_t)(msg[3] | msg[4] << 8);
    const uint8_t *tl = msg + 7;
    size_t tlen = len - 7;
    struct tlv a, b, c;
    struct reply r;
    switch (id) {
    case RMTFS_OPEN: {
        int caller = find_tlv(tl, tlen, 1, &a) ? -1 : do_open(&a);
        reply_start(&r, id, txn, caller < 0, caller < 0);
        if (caller >= 0) { uint32_t v = (uint32_t)caller; reply_tlv(&r, 0x10, &v, 4); }
        break;
    }
    case RMTFS_CLOSE: {
        int ok = !find_tlv(tl, tlen, 1, &a) && a.len >= 4 && get32(a.v) < MAX_CALLERS;
        if (ok) callers[get32(a.v)].open = 0;                  /* the copy stays: what it wrote is there next time */
        reply_start(&r, id, txn, !ok, !ok);
        break;
    }
    case RMTFS_RW_IOVEC: {
        int rc = -1;
        if (!find_tlv(tl, tlen, 1, &a) && !find_tlv(tl, tlen, 2, &b) && !find_tlv(tl, tlen, 3, &c) && a.len >= 4 && b.len >= 1)
            rc = do_iovec((int)get32(a.v), b.v[0], &c);
        if (rc) n_bad++;
        reply_start(&r, id, txn, rc != 0, rc != 0);
        break;
    }
    case RMTFS_ALLOC_BUFF: {
        int ok = !find_tlv(tl, tlen, 2, &b) && b.len >= 4 && get32(b.v) <= RMTFS_MEM_SIZE && mem;
        reply_start(&r, id, txn, !ok, !ok);
        if (ok) { reply_tlv(&r, 0x10, &mem_phys, 8); kprintf("rmtfs: the modem's buffer: %u bytes at %lx\n", get32(b.v), (unsigned long)mem_phys); }
        break;
    }
    case RMTFS_GET_DEV_ERROR: {
        uint8_t st = 0;
        reply_start(&r, id, txn, 0, 0);
        reply_tlv(&r, 0x10, &st, 1);
        break;
    }
    default:
        kprintf("rmtfs: request %u not known\n", id);
        reply_start(&r, id, txn, 1, 2);
        break;
    }
    reply_send(&r, node, port);
}

/* The shared buffer (3 MiB, a guard page each side, as downstream's
 * sharedmem-uio with qcom,guard-memory), handed to the modem's VMIDs
 * through TrustZone, and the service registered: before the modem runs. */
int qcom_rmtfs_setup(void)
{
    if (mem) return 0;
    size_t pages = RMTFS_MEM_SIZE / PAGE_SIZE + 2;
    uint64_t base = pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!base) { kprintf("rmtfs: no 3 MiB of contiguous memory\n"); return -1; }
    mem_phys = base + PAGE_SIZE;
    memset(P2V(mem_phys), 0, RMTFS_MEM_SIZE);
    qcom_dma_clean(P2V(base), pages * PAGE_SIZE);
    static const uint32_t vmids[3] = { SCM_VMID_HLOS, SCM_VMID_MSS_MSA, SCM_VMID_NAV };
    static const uint32_t perms[3] = { SCM_PERM_RW, SCM_PERM_RW, SCM_PERM_RW };
    if (qcom_scm_assign_mem(mem_phys, RMTFS_MEM_SIZE, vmids, perms, 3)) { kprintf("rmtfs: TrustZone would not share the buffer with the modem\n"); return -1; }
    mem = vmm_map_wc(mem_phys, RMTFS_MEM_SIZE);
    if (!mem) return -1;
    qrtr_add_server(RMTFS_SERVICE, RMTFS_VERSION, RMTFS_INSTANCE, RMTFS_PORT, rmtfs_rx);
    kprintf("rmtfs: buffer %lx+%x shared with the modem\n", (unsigned long)mem_phys, RMTFS_MEM_SIZE);
    return 0;
}

size_t qcom_rmtfs_report(char *buf, size_t len)
{
    size_t n = (size_t)ksnprintf(buf, len, "rmtfs: %s, %u opens, %u reads, %u writes (kept in RAM), %u bad\n",
                                 mem ? "serving" : "not set up", n_open, n_read, n_write, n_bad);
    for (int i = 0; i < MAX_CALLERS && n < len; i++)
        if (callers[i].label) n += (size_t)ksnprintf(buf + n, len - n, "  %d: %s%s\n", i, callers[i].label, callers[i].open ? "" : " (closed)");
    return n;
}
