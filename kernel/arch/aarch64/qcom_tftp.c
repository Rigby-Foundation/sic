/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The files a Qualcomm phone's modem asks the apps processor for, over
 * TFTP on QRTR (service 4096), as linux-msm/tqftpserv serves them: its
 * WLAN firmware (wlanmdsp.mbn), its configuration (modem_pr/...) under
 * /readonly/..., from the modem partition's image/ directory; and files it
 * keeps under /readwrite/..., here in RAM (gone at the next boot).
 * Each transfer gets its own port, which the modem's ACKs come to. */
#include "asm/qcom_ipc.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

#define TFTP_SERVICE    4096
#define TFTP_PORT       0x400
#define FW_DIR          "/mnt/modem/image/"
#define MAX_BLKSIZE     8192                    /* what fits the GLINK FIFO easily; RFC 2348 lets us cut it */

enum { OP_RRQ = 1, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR, OP_OACK };
enum { ERR_UNDEF = 0, ERR_ENOENT = 1, ERR_EBADOP = 4, ERR_EOPTNEG = 8, ERR_END_OF_TRANSFER = 9 };

/* /readwrite files */
#define MAX_RW 32
static struct { char path[96]; uint8_t *data; size_t size, cap; } rw[MAX_RW];

struct xfer {
    int used, write;
    uint32_t port, node, rport;
    char path[96];
    struct file *f;                             /* a read from the partition */
    int rwi;                                    /* or a /readwrite file */
    size_t blksize, wsize, rsize, seek, size;
    int done;
};
#define MAX_XFERS 8
static struct xfer xfers[MAX_XFERS];
static uint32_t next_port = TFTP_PORT + 1;
static uint32_t n_reads, n_writes, n_missing;
static char last_files[4][64];
static int last_n;

static size_t str_nlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void send_error(uint32_t sport, uint32_t node, uint32_t port, unsigned code, const char *msg)
{
    uint8_t b[80];
    put16(b, OP_ERROR); put16(b + 2, code);
    size_t n = strlen(msg);
    if (n > 70) n = 70;
    memcpy(b + 4, msg, n); b[4 + n] = 0;
    qrtr_sendto(sport, node, port, b, 5 + n);
}

static int rw_find(const char *path)
{
    for (int i = 0; i < MAX_RW; i++) if (rw[i].data && strcmp(rw[i].path, path) == 0) return i;
    return -1;
}

/* /readonly/firmware/image/x, /readonly/firmware/x, /readonly/vendor/firmware(_mnt/image)/x: x on the partition */
static const char *readonly_name(const char *p)
{
    static const char *const prefixes[] = { "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/",
                                            "/readonly/vendor/firmware/", "/readonly/firmware/" };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (prefix(p, prefixes[i])) return p + n;
    }
    return NULL;
}

static uint64_t parse_num(const char *s) { uint64_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0'); return v; }

static size_t xfer_read(struct xfer *x, size_t off, uint8_t *dst, size_t n)
{
    if (x->rwi >= 0) {
        if (off >= rw[x->rwi].size) return 0;
        if (n > rw[x->rwi].size - off) n = rw[x->rwi].size - off;
        memcpy(dst, rw[x->rwi].data + off, n);
        return n;
    }
    if (file_seek(x->f, (long)off, 0) < 0) return 0;
    long got = file_read(x->f, dst, n);
    return got > 0 ? (size_t)got : 0;
}

static void xfer_close(struct xfer *x)
{
    if (x->f) file_close(x->f);
    qrtr_unbind_port(x->port);
    x->used = 0;
}

/* Blocks after `last`, up to the window: block k carries bytes
 * seek + (k-1)*blksize on, as tqftpserv; a short block ends it. */
static void send_blocks(struct xfer *x, unsigned last)
{
    size_t limit = x->rsize ? x->rsize : (x->size > x->seek ? x->size - x->seek : 0);
    uint8_t *b = kmalloc(x->blksize + 4);
    if (!b) return;
    for (unsigned k = last + 1; k <= last + x->wsize && !x->done; k++) {
        size_t start = (size_t)(k - 1) * x->blksize, n = 0;
        if (start < limit) {
            n = limit - start < x->blksize ? limit - start : x->blksize;
            n = xfer_read(x, x->seek + start, b + 4, n);
        }
        put16(b, OP_DATA); put16(b + 2, k & 0xffff);
        qrtr_sendto(x->port, x->node, x->rport, b, 4 + n);
        if (n < x->blksize) x->done = 1;
    }
    kfree(b);
}

static void xfer_rx(uint32_t node, uint32_t port, const uint8_t *d, size_t len);

static struct xfer *xfer_by_port(uint32_t port)
{
    for (int i = 0; i < MAX_XFERS; i++) if (xfers[i].used && xfers[i].port == port) return &xfers[i];
    return NULL;
}

static void request(uint32_t node, uint32_t port, const uint8_t *d, size_t len, int write)
{
    const char *end = (const char *)d + len, *path = (const char *)d + 2;
    size_t pl = str_nlen(path, (size_t)(end - path));
    if (pl == (size_t)(end - path) || pl >= 96) return;
    const char *mode = path + pl + 1;
    if (mode >= end) return;
    const char *opt = mode + str_nlen(mode, (size_t)(end - mode)) + 1;

    struct xfer *x = NULL;
    for (int i = 0; i < MAX_XFERS && !x; i++) if (!xfers[i].used) x = &xfers[i];
    if (!x) { send_error(TFTP_PORT, node, port, ERR_UNDEF, "busy"); return; }
    memset(x, 0, sizeof *x);
    x->used = 1; x->write = write; x->node = node; x->rport = port; x->rwi = -1;
    x->blksize = 512; x->wsize = 1;
    memcpy(x->path, path, pl + 1);
    x->port = next_port++;
    if (next_port > TFTP_PORT + 0x3ff) next_port = TFTP_PORT + 1;

    int has_opts = opt < end, want_tsize = 0;
    for (const char *o = opt; o < end; ) {
        const char *v = o + str_nlen(o, (size_t)(end - o)) + 1;
        if (v >= end) break;
        uint64_t n = parse_num(v);
        if (!strcmp(o, "blksize")) x->blksize = n < 8 ? 8 : n > MAX_BLKSIZE ? MAX_BLKSIZE : n;
        else if (!strcmp(o, "wsize")) x->wsize = n ? n : 1;
        else if (!strcmp(o, "rsize")) x->rsize = n;
        else if (!strcmp(o, "seek")) x->seek = n;
        else if (!strcmp(o, "tsize")) want_tsize = 1;
        o = v + str_nlen(v, (size_t)(end - v)) + 1;
    }

    if (prefix(path, "/readwrite/")) {
        x->rwi = rw_find(path);
        if (write && x->rwi < 0)
            for (int i = 0; i < MAX_RW && x->rwi < 0; i++)
                if (!rw[i].data) { memcpy(rw[i].path, path, pl + 1); rw[i].data = kmalloc(4096); rw[i].cap = 4096; rw[i].size = 0; if (rw[i].data) x->rwi = i; }
        if (x->rwi >= 0 && write) rw[x->rwi].size = x->seek;     /* a write starts it over (at its seek) */
        if (x->rwi >= 0) x->size = rw[x->rwi].size;
    } else if (!write) {
        const char *name = readonly_name(path);
        char full[160];
        if (name) { ksnprintf(full, sizeof full, FW_DIR "%s", name); x->f = vfs_open(vfs_root(), full, O_RDONLY); }
        if (x->f) { struct stat st; vnode_stat(x->f->node, &st); x->size = st.size; }
    }
    if ((x->rwi < 0 && !x->f) || (write && x->rwi < 0)) {
        n_missing++;
        send_error(TFTP_PORT, node, port, ERR_ENOENT, "file not found");
        x->used = 0;
        return;
    }
    if (write) n_writes++; else n_reads++;
    ksnprintf(last_files[last_n++ % 4], sizeof last_files[0], "%s %s", write ? "W" : "R", path);
    qrtr_bind_port(x->port, xfer_rx);

    if (has_opts) {                                             /* OACK: what we took of its options */
        uint8_t b[160];
        size_t n = 2;
        put16(b, OP_OACK);
        n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "blksize") + 1;
        n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "%lu", (unsigned long)x->blksize) + 1;
        if (want_tsize) {
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "tsize") + 1;
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "%lu", (unsigned long)x->size) + 1;
        }
        n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "wsize") + 1;
        n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "%lu", (unsigned long)x->wsize) + 1;
        if (x->rsize) {
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "rsize") + 1;
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "%lu", (unsigned long)x->rsize) + 1;
        }
        if (x->seek) {
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "seek") + 1;
            n += (size_t)ksnprintf((char *)b + n, sizeof b - n, "%lu", (unsigned long)x->seek) + 1;
        }
        qrtr_sendto(x->port, node, port, b, n);
    } else if (write) {
        uint8_t b[4]; put16(b, OP_ACK); put16(b + 2, 0);
        qrtr_sendto(x->port, node, port, b, 4);
    } else {
        send_blocks(x, 0);
    }
}

/* ACKs (a read) or DATA (a write) to a transfer's port. */
static void xfer_rx(uint32_t node, uint32_t port, const uint8_t *d, size_t len)
{
    struct xfer *x = xfer_by_port(qrtr_rx_port());
    if (!x || x->node != node || x->rport != port || len < 4) return;
    unsigned op = (unsigned)d[0] << 8 | d[1], block = (unsigned)d[2] << 8 | d[3];
    if (op == OP_ERROR) { xfer_close(x); return; }             /* "end of transfer", or it gave up */
    if (!x->write && op == OP_ACK) {
        if (x->done) { xfer_close(x); return; }
        send_blocks(x, block);
        return;
    }
    if (x->write && op == OP_DATA) {
        size_t n = len - 4, off = x->seek + (size_t)(block - 1) * x->blksize;
        if (off + n > rw[x->rwi].cap) {
            size_t cap = (off + n) * 2;
            uint8_t *nd = kmalloc(cap);
            if (!nd) { send_error(x->port, x->node, x->rport, ERR_UNDEF, "no memory"); xfer_close(x); return; }
            memcpy(nd, rw[x->rwi].data, rw[x->rwi].size);
            kfree(rw[x->rwi].data);
            rw[x->rwi].data = nd; rw[x->rwi].cap = cap;
        }
        memcpy(rw[x->rwi].data + off, d + 4, n);
        if (off + n > rw[x->rwi].size) rw[x->rwi].size = off + n;
        uint8_t b[4]; put16(b, OP_ACK); put16(b + 2, block);
        qrtr_sendto(x->port, x->node, x->rport, b, 4);
        if (n < x->blksize) xfer_close(x);
    }
}

static void tftp_rx(uint32_t node, uint32_t port, const uint8_t *d, size_t len)
{
    if (len < 4) return;
    unsigned op = (unsigned)d[0] << 8 | d[1];
    if (op == OP_RRQ || op == OP_WRQ) request(node, port, d, len, op == OP_WRQ);
    else send_error(TFTP_PORT, node, port, ERR_EBADOP, "expected RRQ or WRQ");
}

void qcom_tftp_setup(void)
{
    static int done;
    if (done++) return;
    qrtr_add_server(TFTP_SERVICE, 1, 0, TFTP_PORT, tftp_rx);
}

size_t qcom_tftp_report(char *buf, size_t len)
{
    size_t n = (size_t)ksnprintf(buf, len, "tftp: %u reads, %u writes, %u not found; last:", n_reads, n_writes, n_missing);
    for (int i = 0; i < 4 && i < last_n && n < len; i++) n += (size_t)ksnprintf(buf + n, len - n, " %s", last_files[(last_n - 1 - i) % 4]);
    if (n < len) n += (size_t)ksnprintf(buf + n, len - n, "\n");
    return n;
}
