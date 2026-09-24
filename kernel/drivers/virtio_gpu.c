/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* virtio-gpu, 2D: the display adapter of a virtual machine. The framebuffer
 * is ordinary RAM (a host resource with our pages as backing); the host
 * shows it after TRANSFER_TO_HOST + FLUSH, which a kernel thread issues at
 * a fixed rate for the whole screen. That is what makes the console and
 * /dev/fb0 work unchanged on top: they write pixels, this pushes them.
 *
 * QEMU: -device virtio-gpu-pci (no VGA compatibility: the firmware shows
 * nothing until we're up) or -vga virtio / -device virtio-vga (the boot
 * framebuffer works until this driver takes the display over). */
#include "drivers/virtio.h"
#include "drivers/fb.h"
#include "drivers/virtio_gpu.h"
#include "abi/gpu.h"
#include "abi/abi.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "asm/irq.h"
#include "proc/wait.h"

#define CMD_GET_DISPLAY_INFO        0x0100
#define CMD_RESOURCE_CREATE_2D      0x0101
#define CMD_SET_SCANOUT             0x0103
#define CMD_RESOURCE_FLUSH          0x0104
#define CMD_TRANSFER_TO_HOST_2D     0x0105
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_GET_CAPSET_INFO         0x0108
#define CMD_GET_CAPSET              0x0109
/* 3D */
#define CMD_CTX_CREATE              0x0200
#define CMD_CTX_DESTROY             0x0201
#define CMD_CTX_ATTACH_RESOURCE     0x0202
#define CMD_CTX_DETACH_RESOURCE     0x0203
#define CMD_RESOURCE_CREATE_3D      0x0204
#define CMD_TRANSFER_TO_HOST_3D     0x0205
#define CMD_TRANSFER_FROM_HOST_3D   0x0206
#define CMD_SUBMIT_3D               0x0207
#define CMD_RESOURCE_UNREF          0x0102
#define RESP_OK_NODATA              0x1100
#define RESP_OK_DISPLAY_INFO        0x1101
#define RESP_OK_CAPSET_INFO         0x1102
#define RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_F_VIRGL          (1ULL << 0)
#define GPU_FLAG_FENCE              1
#define GPU_FLAG_INFO_RING_IDX      2       /* the fence belongs to the context (ring_idx), not the device */

struct gpu_hdr  { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; } __attribute__((packed));
struct gpu_rect { uint32_t x, y, w, h; } __attribute__((packed));
struct gpu_display_info { struct gpu_hdr hdr; struct { struct gpu_rect r; uint32_t enabled, flags; } pmodes[16]; } __attribute__((packed));
struct gpu_create_2d { struct gpu_hdr hdr; uint32_t resource_id, format, width, height; } __attribute__((packed));
struct gpu_set_scanout { struct gpu_hdr hdr; struct gpu_rect r; uint32_t scanout_id, resource_id; } __attribute__((packed));
struct gpu_flush { struct gpu_hdr hdr; struct gpu_rect r; uint32_t resource_id, pad; } __attribute__((packed));
struct gpu_transfer_2d { struct gpu_hdr hdr; struct gpu_rect r; uint64_t offset; uint32_t resource_id, pad; } __attribute__((packed));
struct gpu_attach { struct gpu_hdr hdr; uint32_t resource_id, nr_entries; uint64_t addr; uint32_t length, pad; } __attribute__((packed));
struct gpu_box { uint32_t x, y, z, w, h, d; } __attribute__((packed));
struct gpu_ctx_create { struct gpu_hdr hdr; uint32_t nlen, context_init; char debug_name[64]; } __attribute__((packed));
struct gpu_ctx_res { struct gpu_hdr hdr; uint32_t resource_id, pad; } __attribute__((packed));
struct gpu_create_3d { struct gpu_hdr hdr; uint32_t resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, pad; } __attribute__((packed));
struct gpu_transfer_3d { struct gpu_hdr hdr; struct gpu_box box; uint64_t offset; uint32_t resource_id, level, stride, layer_stride; } __attribute__((packed));
struct gpu_submit_3d { struct gpu_hdr hdr; uint32_t size, pad; } __attribute__((packed));
struct gpu_capset_info { struct gpu_hdr hdr; uint32_t capset_index, pad; } __attribute__((packed));
struct gpu_resp_capset_info { struct gpu_hdr hdr; uint32_t capset_id, capset_max_version, capset_max_size, pad; } __attribute__((packed));
struct gpu_get_capset { struct gpu_hdr hdr; uint32_t capset_id, capset_version; } __attribute__((packed));

/* 0x00RRGGBB in a uint32_t is B,G,R,X in memory on a little-endian CPU. */
#if SIC_BIG_ENDIAN_CPU
#define FORMAT_XRGB 4       /* X8R8G8B8 */
#else
#define FORMAT_XRGB 2       /* B8G8R8X8 */
#endif

static struct virtio_dev dev;
static struct virtq ctrlq;
static uint8_t *cmdbuf;                     /* one direct-mapped page for a command and its reply */
static uint32_t width, height;
static uint64_t fb_phys;
/* The framebuffer is one fixed allocation big enough for any mode the host
 * may ask for: a resize is a new scanout resource on the same memory, so
 * whoever mapped /dev/fb0 keeps a valid mapping. */
#define FB_MAX_BYTES (4096u * 2304u * 4u)   /* room for a Retina panel at its real size (a 16" MacBook Pro is 3456x2234) */
static uint32_t scanout_res = 1;
static int up, has_virgl;
static void virgl_init(void);
static uint32_t next_res_id = 2, next_ctx_id = 1;
static uint32_t capset_id, capset_version, capset_size;
/* One command at a time (the present thread, FBIOPRESENT, /dev/gpu0), and
 * the waiting is sleeping, not spinning: a GL frame's readback or a whole
 * screen's upload takes the host milliseconds, and a spinlock around that
 * had everybody else burning a CPU until it finished. */
struct gpu_file;
static struct gpu_res *all_res;         /* under gpu_take */
static struct gpu_file *hw_owner;       /* the file whose resource the display shows */
static uint32_t hw_res;

static spinlock_t gpu_flag_lock = SPINLOCK_INIT;
static int gpu_busy;
static struct waitqueue gpu_free = WAITQUEUE_INIT;     /* the lock is free */

static void gpu_take(void)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&gpu_flag_lock);
        if (!gpu_busy) { gpu_busy = 1; spin_unlock_irqrestore(&gpu_flag_lock, f); return; }
        spin_unlock_irqrestore(&gpu_flag_lock, f);
        if (sched_started()) wait_event_timeout(&gpu_free, !gpu_busy, 10);
    }
}

static void gpu_give(void)
{
    uint64_t f = spin_lock_irqsave(&gpu_flag_lock);
    gpu_busy = 0;
    spin_unlock_irqrestore(&gpu_flag_lock, f);
    waitqueue_wake_all(&gpu_free);
}

static void gpu_irq(struct interrupt_frame *f)
{
    (void)f;
    extern unsigned long virtio_irqs;
    virtio_irqs++;
    (void)mmio_read8(dev.isr);                  /* acknowledges the line */
}
static uint8_t *submitbuf;                      /* GPU_SUBMIT_MAX + header, direct-mapped */
#define SUBMIT_PAGES (PAGE_ALIGN_UP(GPU_SUBMIT_MAX + 4096) / PAGE_SIZE)

static void hdr(void *cmd, uint32_t type) { struct gpu_hdr *h = cmd; memset(h, 0, sizeof *h); h->type = htole32(type); }
static void hdr_ctx(void *cmd, uint32_t type, uint32_t ctx) { hdr(cmd, type); ((struct gpu_hdr *)cmd)->ctx_id = htole32(ctx); }

/* Issue a command; the reply lands in cmdbuf + 2048. Returns the reply type or 0. */
static uint32_t cmd(size_t len)
{
    struct gpu_hdr *resp = (void *)(cmdbuf + 2048);
    memset(resp, 0, 2048);
    if (virtio_request(&ctrlq, cmdbuf, len, resp, 2048) < 0) return 0;
    return le32toh(resp->type);
}

static void set_rect(struct gpu_rect *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    r->x = htole32(x); r->y = htole32(y); r->w = htole32(w); r->h = htole32(h);
}

/* What has changed since the last push: the present thread sends just
 * that (a whole screen at 2560x1600 is 16 MB per push; a cursor move is
 * a few hundred bytes). */
static spinlock_t dirty_lock = SPINLOCK_INIT;
static uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static struct waitqueue present_wq = WAITQUEUE_INIT;

static void note_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint64_t f = spin_lock_irqsave(&dirty_lock);
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) { dirty_x0 = x; dirty_y0 = y; dirty_x1 = x + w; dirty_y1 = y + h; }
    else {
        if (x < dirty_x0) dirty_x0 = x;
        if (y < dirty_y0) dirty_y0 = y;
        if (x + w > dirty_x1) dirty_x1 = x + w;
        if (y + h > dirty_y1) dirty_y1 = y + h;
    }
    spin_unlock_irqrestore(&dirty_lock, f);
}

/* The console's: it may run under the scheduler's lock (a kprintf there), so
 * no wakeup; the thread looks every few milliseconds anyway. */
static void console_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { if (up) note_dirty(x, y, w, h); }

static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    note_dirty(x, y, w, h);
    waitqueue_wake_all(&present_wq);
}

static int take_dirty(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    uint64_t f = spin_lock_irqsave(&dirty_lock);
    int any = dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0;
    if (any) {
        if (dirty_x1 > width) dirty_x1 = width;
        if (dirty_y1 > height) dirty_y1 = height;
        *x = dirty_x0; *y = dirty_y0; *w = dirty_x1 - dirty_x0; *h = dirty_y1 - dirty_y0;
        any = *w > 0 && *h > 0;
    }
    dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
    spin_unlock_irqrestore(&dirty_lock, f);
    return any;
}

static void present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (hw_owner) return;                       /* a compositor has the display */
    gpu_take();
    struct gpu_transfer_2d *t = (void *)cmdbuf;
    hdr(t, CMD_TRANSFER_TO_HOST_2D);
    set_rect(&t->r, x, y, w, h);
    t->offset = htole64((uint64_t)y * width * 4 + (uint64_t)x * 4); t->resource_id = htole32(scanout_res); t->pad = 0;
    if (cmd(sizeof *t) == RESP_OK_NODATA) {
        struct gpu_flush *f = (void *)cmdbuf;
        hdr(f, CMD_RESOURCE_FLUSH);
        set_rect(&f->r, x, y, w, h);
        f->resource_id = htole32(scanout_res); f->pad = 0;
        cmd(sizeof *f);
    }
    gpu_give();
}

static void present(void) { present_rect(0, 0, width, height); }

/* Ask the host for its display size; 0 if it has none. */
static int query_mode(uint32_t *w, uint32_t *h)
{
    hdr(cmdbuf, CMD_GET_DISPLAY_INFO);
    if (cmd(sizeof(struct gpu_hdr)) != RESP_OK_DISPLAY_INFO) return 0;
    struct gpu_display_info *di = (void *)(cmdbuf + 2048);
    if (!le32toh(di->pmodes[0].enabled) || le32toh(di->pmodes[0].r.w) < 320 || le32toh(di->pmodes[0].r.h) < 200) return 0;
    *w = le32toh(di->pmodes[0].r.w);
    *h = le32toh(di->pmodes[0].r.h);
    return 1;
}

/* A 2D resource of the display's size on the framebuffer memory, scanned out. */
static int make_scanout(uint32_t w, uint32_t h)
{
    uint32_t id = next_res_id++;
    struct gpu_create_2d *c = (void *)cmdbuf;
    hdr(c, CMD_RESOURCE_CREATE_2D);
    c->resource_id = htole32(id); c->format = htole32(FORMAT_XRGB); c->width = htole32(w); c->height = htole32(h);
    if (cmd(sizeof *c) != RESP_OK_NODATA) { kprintf("virtio-gpu: resource create failed\n"); return -1; }
    struct gpu_attach *a = (void *)cmdbuf;
    hdr(a, CMD_RESOURCE_ATTACH_BACKING);
    a->resource_id = htole32(id); a->nr_entries = htole32(1);
    a->addr = htole64(fb_phys); a->length = htole32(PAGE_ALIGN_UP((uint32_t)w * h * 4)); a->pad = 0;
    if (cmd(sizeof *a) != RESP_OK_NODATA) { kprintf("virtio-gpu: attach backing failed\n"); return -1; }
    struct gpu_set_scanout *s = (void *)cmdbuf;
    hdr(s, CMD_SET_SCANOUT);
    set_rect(&s->r, 0, 0, w, h);
    s->scanout_id = 0; s->resource_id = htole32(id);
    if (!hw_owner && cmd(sizeof *s) != RESP_OK_NODATA) { kprintf("virtio-gpu: set scanout failed\n"); return -1; }   /* a compositor keeps the display */
    if (scanout_res && scanout_res != id) {
        struct gpu_ctx_res *u = (void *)cmdbuf;     /* same shape: header + resource id */
        hdr(u, CMD_RESOURCE_UNREF);
        u->resource_id = htole32(scanout_res); u->pad = 0;
        cmd(sizeof *u);
    }
    scanout_res = id;
    width = w; height = h;
    return 0;
}

/* The host's window was resized (VIRTIO_GPU_EVENT_DISPLAY in the config
 * space): follow it, within the buffer we have. */
static void check_resize(void)
{
    if (!(virtio_cfg_read32(&dev, 0) & 1)) return;          /* events_read */
    virtio_cfg_write32(&dev, 4, 1);                          /* events_clear */
    gpu_take();
    uint32_t w, h;
    if (query_mode(&w, &h) && (w != width || h != height)) {
        if ((uint64_t)w * h * 4 > FB_MAX_BYTES) {
            kprintf("virtio-gpu: %ux%u does not fit the framebuffer, staying at %ux%u\n", w, h, width, height);
        } else if (make_scanout(w, h) == 0) {
            memset(P2V(fb_phys), 0, (size_t)w * h * 4);
            fb_mode_change(w, h, w * 4, FB_MAX_BYTES);
            kprintf("virtio-gpu: display now %ux%u\n", w, h);
        }
    }
    gpu_give();
}

/* Pushes what is dirty as soon as it is (a compositor's flushes, console
 * output), and looks for a resized window now and then. */
static void present_thread(void *arg)
{
    (void)arg;
    uint64_t last_check = 0;
    for (;;) {
        uint32_t x, y, w, h;
        wait_event_timeout(&present_wq, dirty_x1 > dirty_x0, 15);   /* console output only marks: pick it up within 15 ms */
        if (take_dirty(&x, &y, &w, &h)) present_rect(x, y, w, h);
        if (timer_ticks() - last_check >= 250) { last_check = timer_ticks(); check_resize(); }
    }
}

static int virtio_gpu_present_rect_now(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!up) return -1;
    mark_dirty(x, y, w, h);
    return 0;
}

void virtio_gpu_init(void)
{
    const struct pci_dev *pd = NULL;
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_dev *p = pci_get(i);
        if (p->vendor == VIRTIO_PCI_VENDOR && (p->device == 0x1040 + VIRTIO_ID_GPU || p->device == 0x1050)) { pd = p; break; }
    }
    if (!pd) return;
    if (virtio_init(&dev, pd, VIRTIO_GPU_F_VIRGL) != 0) return;
    has_virgl = virtio_cfg_read32(&dev, 12) > 0;             /* config: num_capsets, the host offers 3D */
    if (virtio_queue_setup(&dev, &ctrlq, 0) != 0) { kprintf("virtio-gpu: no control queue\n"); return; }
    {
        int irq = pci_irq(pd);
        if (irq >= 0) { irq_install((uint8_t)irq, gpu_irq); irq_unmask_pci((uint8_t)irq); }
    }
    virtio_driver_ok(&dev);
    cmdbuf = P2V(pmm_alloc_page());

    /* the host's idea of the display size, else 1024x768 */
    uint32_t w = 1024, h = 768;
    if (query_mode(&w, &h) && (uint64_t)w * h * 4 > FB_MAX_BYTES) { w = 1024; h = 768; }

    fb_phys = pmm_alloc_pages(FB_MAX_BYTES / PAGE_SIZE);
    if (!fb_phys) { kprintf("virtio-gpu: no memory for the framebuffer\n"); return; }
    memset(P2V(fb_phys), 0, FB_MAX_BYTES);
    scanout_res = 0;
    if (make_scanout(w, h) != 0) return;

    /* Hand the framebuffer to the console and /dev/fb0. */
    struct zaeboot_framebuffer fb = {
        .base = fb_phys, .width = width, .height = height, .pitch = width * 4, .bpp = 32,
        .red_shift = 16, .green_shift = 8, .blue_shift = 0,
    };
    fb_init(&fb, P2V(fb_phys), 1);
    fb_mode_change(width, height, width * 4, FB_MAX_BYTES);
    fb_set_present_hook(virtio_gpu_present_now);
    fb_set_present_rect_hook(virtio_gpu_present_rect_now);
    fb_set_damage_hook(console_damage);
    up = 1;
    kprintf("virtio-gpu: %ux%u at %02x:%02x.%u, framebuffer in RAM at %llx\n", width, height, pd->bus, pd->slot, pd->func, fb_phys);
    present();
    task_create("gpu-present", present_thread, NULL);
    if (has_virgl)
        virgl_init();
}

/* From anywhere (kprintf's console hook included): mark, and let the thread push. */
int virtio_gpu_present_now(void)
{
    if (!up) return -1;
    mark_dirty(0, 0, width, height);
    return 0;
}

/* ---- 3D: virgl contexts, resources, command streams, /dev/gpu0 ---------------- */

struct gpu_res {
    uint32_t id;
    uint64_t phys;              /* guest backing, contiguous */
    size_t pages;
    int refs;                   /* the owner, attachers, the scanout */
    struct gpu_res *next;       /* the owner's list */
    struct gpu_res *gnext;      /* every resource */
};

struct gpu_attached { uint32_t id; struct gpu_attached *next; };

struct gpu_file {
    uint32_t ctx;
    struct gpu_res *res;
    struct gpu_attached *attached;      /* other files' resources used here */
};


static struct gpu_res *res_global(uint32_t id)
{
    for (struct gpu_res *r = all_res; r; r = r->gnext) if (r->id == id) return r;
    return NULL;
}

/* One reference gone (gpu_take held); the last one frees it on the host and here. */
static void res_unref_locked(struct gpu_res *r)
{
    if (--r->refs > 0) return;
    struct gpu_ctx_res *d = (void *)cmdbuf;
    hdr(d, CMD_RESOURCE_UNREF);
    d->resource_id = htole32(r->id); d->pad = 0;
    cmd(sizeof *d);
    for (struct gpu_res **pp = &all_res; *pp; pp = &(*pp)->gnext) if (*pp == r) { *pp = r->gnext; break; }
    if (r->pages) pmm_free_pages(r->phys, r->pages);
    kfree(r);
}

static void ctx_res(uint32_t type, uint32_t ctx, uint32_t id)
{
    struct gpu_ctx_res *d = (void *)cmdbuf;
    hdr_ctx(d, type, ctx);
    d->resource_id = htole32(id); d->pad = 0;
    cmd(sizeof *d);
}

static void virgl_init(void)
{
    struct gpu_capset_info *ci = (void *)cmdbuf;
    hdr(ci, CMD_GET_CAPSET_INFO);
    ci->capset_index = 0; ci->pad = 0;
    if (cmd(sizeof *ci) == RESP_OK_CAPSET_INFO) {
        struct gpu_resp_capset_info *r = (void *)(cmdbuf + 2048);
        capset_id = le32toh(r->capset_id);
        capset_version = le32toh(r->capset_max_version);
        capset_size = le32toh(r->capset_max_size);
    }
    /* prefer VIRGL2 (capset 2) when the host lists it */
    uint32_t n = virtio_cfg_read32(&dev, 12);
    for (uint32_t i = 1; i < n && i < 4; i++) {
        hdr(ci, CMD_GET_CAPSET_INFO);
        ci->capset_index = i; ci->pad = 0;
        if (cmd(sizeof *ci) != RESP_OK_CAPSET_INFO) continue;
        struct gpu_resp_capset_info *r = (void *)(cmdbuf + 2048);
        if (le32toh(r->capset_id) == 2) {
            capset_id = 2; capset_version = le32toh(r->capset_max_version); capset_size = le32toh(r->capset_max_size);
        }
    }
    submitbuf = P2V(pmm_alloc_pages(SUBMIT_PAGES));
    if (!submitbuf) { has_virgl = 0; return; }
    extern const struct dev_ops gpu_dev_ops;
    vfs_mkdev("/dev/gpu0", &gpu_dev_ops, NULL);
    kprintf("virtio-gpu: 3D (virgl) available, capset %u v%u -> /dev/gpu0\n", capset_id, capset_version);
}

static struct gpu_file *gpu_file_of(struct file *f) { return f->priv_gpu; }

static long gpu_dev_open(struct file *f)
{
    struct gpu_file *g = kzalloc(sizeof *g);
    if (!g) return -ENOMEM;
    g->ctx = next_ctx_id++;
    struct gpu_ctx_create *c = (void *)cmdbuf;
    gpu_take();
    hdr_ctx(c, CMD_CTX_CREATE, g->ctx);
    c->nlen = htole32(3); c->context_init = 0;
    memset(c->debug_name, 0, sizeof c->debug_name);
    memcpy(c->debug_name, "sic", 3);
    uint32_t r = cmd(sizeof *c);
    gpu_give();
    if (r != RESP_OK_NODATA) { kfree(g); return -EIO; }
    f->priv_gpu = g;
    return 0;
}

static void res_destroy(struct gpu_file *g, struct gpu_res *r)
{
    gpu_take();
    ctx_res(CMD_CTX_DETACH_RESOURCE, g->ctx, r->id);
    res_unref_locked(r);
    gpu_give();
}

static long res_attach(struct gpu_file *g, uint32_t id)
{
    struct gpu_attached *a = kzalloc(sizeof *a);
    if (!a) return -ENOMEM;
    gpu_take();
    struct gpu_res *r = res_global(id);
    if (!r) { gpu_give(); kfree(a); return -ENOENT; }
    r->refs++;
    ctx_res(CMD_CTX_ATTACH_RESOURCE, g->ctx, id);
    gpu_give();
    a->id = id; a->next = g->attached; g->attached = a;
    return 0;
}

static long res_detach(struct gpu_file *g, uint32_t id)
{
    for (struct gpu_attached **pp = &g->attached; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            struct gpu_attached *a = *pp;
            *pp = a->next;
            kfree(a);
            gpu_take();
            ctx_res(CMD_CTX_DETACH_RESOURCE, g->ctx, id);
            struct gpu_res *r = res_global(id);
            if (r) res_unref_locked(r);
            gpu_give();
            return 0;
        }
    return -ENOENT;
}

static void scanout_back_to_framebuffer(void);

static void gpu_dev_release(struct file *f)
{
    struct gpu_file *g = gpu_file_of(f);
    if (!g) return;
    if (hw_owner == g) scanout_back_to_framebuffer();
    while (g->attached) res_detach(g, g->attached->id);
    while (g->res) {
        struct gpu_res *r = g->res;
        g->res = r->next;
        res_destroy(g, r);
    }
    struct gpu_hdr *h = (void *)cmdbuf;
    gpu_take();
    hdr_ctx(h, CMD_CTX_DESTROY, g->ctx);
    cmd(sizeof *h);
    gpu_give();
    kfree(g);
    f->priv_gpu = NULL;
}

static struct gpu_res *res_find(struct gpu_file *g, uint32_t id)
{
    for (struct gpu_res *r = g->res; r; r = r->next)
        if (r->id == id) return r;
    return NULL;
}

static long gpu_create_res(struct gpu_file *g, struct gpu_res_create *rc)
{
    struct gpu_res *r = kzalloc(sizeof *r);
    if (!r) return -ENOMEM;
    r->id = next_res_id++;
    if (rc->size) {
        r->pages = PAGE_ALIGN_UP(rc->size) / PAGE_SIZE;
        r->phys = pmm_alloc_pages(r->pages);
        if (!r->phys) { kfree(r); return -ENOMEM; }
        memset(P2V(r->phys), 0, r->pages * PAGE_SIZE);
    }
    gpu_take();
    struct gpu_create_3d *c = (void *)cmdbuf;
    hdr_ctx(c, CMD_RESOURCE_CREATE_3D, g->ctx);
    c->resource_id = htole32(r->id);
    c->target = htole32(rc->target); c->format = htole32(rc->format); c->bind = htole32(rc->bind);
    c->width = htole32(rc->width); c->height = htole32(rc->height); c->depth = htole32(rc->depth);
    c->array_size = htole32(rc->array_size); c->last_level = htole32(rc->last_level);
    c->nr_samples = htole32(rc->nr_samples); c->flags = htole32(rc->flags); c->pad = 0;
    uint32_t ok = cmd(sizeof *c) == RESP_OK_NODATA;
    if (ok && r->pages) {
        struct gpu_attach *a = (void *)cmdbuf;
        hdr_ctx(a, CMD_RESOURCE_ATTACH_BACKING, g->ctx);
        a->resource_id = htole32(r->id); a->nr_entries = htole32(1);
        a->addr = htole64(r->phys); a->length = htole32((uint32_t)(r->pages * PAGE_SIZE)); a->pad = 0;
        ok = cmd(sizeof *a) == RESP_OK_NODATA;
    }
    if (ok) {
        struct gpu_ctx_res *at = (void *)cmdbuf;
        hdr_ctx(at, CMD_CTX_ATTACH_RESOURCE, g->ctx);
        at->resource_id = htole32(r->id); at->pad = 0;
        ok = cmd(sizeof *at) == RESP_OK_NODATA;
    }
    gpu_give();
    if (!ok) {
        if (r->pages) pmm_free_pages(r->phys, r->pages);
        kfree(r);
        return -EIO;
    }
    r->refs = 1;
    r->next = g->res;
    g->res = r;
    gpu_take();
    r->gnext = all_res; all_res = r;
    gpu_give();
    rc->id = r->id;
    return 0;
}

static void set_scanout_locked(uint32_t res, uint32_t w, uint32_t h)
{
    struct gpu_set_scanout *s = (void *)cmdbuf;
    hdr(s, CMD_SET_SCANOUT);
    set_rect(&s->r, 0, 0, w, h);
    s->scanout_id = 0; s->resource_id = htole32(res);
    cmd(sizeof *s);
}

static void flush_locked(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct gpu_flush *fl = (void *)cmdbuf;
    hdr(fl, CMD_RESOURCE_FLUSH);
    set_rect(&fl->r, x, y, w, h);
    fl->resource_id = htole32(res); fl->pad = 0;
    cmd(sizeof *fl);
}

/* The compositor went away: the framebuffer is what the display shows again. */
static void scanout_back_to_framebuffer(void)
{
    gpu_take();
    struct gpu_res *r = res_global(hw_res);
    hw_owner = NULL; hw_res = 0;
    set_scanout_locked(scanout_res, width, height);
    if (r) res_unref_locked(r);
    gpu_give();
    mark_dirty(0, 0, width, height);
}

static long gpu_set_scanout(struct gpu_file *g, const struct gpu_scanout *so)
{
    gpu_take();
    struct gpu_res *r = res_global(so->res), *old = hw_owner ? res_global(hw_res) : NULL;
    if (!r || (hw_owner && hw_owner != g)) { gpu_give(); return r ? -EBUSY : -ENOENT; }
    r->refs++;
    set_scanout_locked(so->res, so->w, so->h);
    hw_owner = g; hw_res = so->res;
    if (old) res_unref_locked(old);
    gpu_give();
    return 0;
}

static long gpu_transfer(struct gpu_file *g, const struct gpu_transfer *t, int to_host)
{
    struct gpu_res *r = res_find(g, t->res);
    if (!r) return -ENOENT;
    struct gpu_transfer_3d *c = (void *)cmdbuf;
    gpu_take();
    hdr_ctx(c, to_host ? CMD_TRANSFER_TO_HOST_3D : CMD_TRANSFER_FROM_HOST_3D, g->ctx);
    c->box.x = htole32(t->x); c->box.y = htole32(t->y); c->box.z = htole32(t->z);
    c->box.w = htole32(t->w); c->box.h = htole32(t->h); c->box.d = htole32(t->d);
    c->offset = htole64(t->offset); c->resource_id = htole32(t->res); c->level = htole32(t->level);
    c->stride = htole32(t->stride); c->layer_stride = htole32(t->layer_stride);
    uint32_t rr = cmd(sizeof *c);
    gpu_give();
    return rr == RESP_OK_NODATA ? 0 : -EIO;
}

static long gpu_submit(struct gpu_file *g, const struct gpu_submit *sb)
{
    if (sb->size == 0 || sb->size > GPU_SUBMIT_MAX || (sb->size & 3)) return -EINVAL;
    if (!user_ok(sb->buf, sb->size)) return -EFAULT;
    gpu_take();
    struct gpu_submit_3d *c = (void *)submitbuf;
    hdr_ctx(c, CMD_SUBMIT_3D, g->ctx);
    if (sb->flags & GPU_SUBMIT_FENCE) {
        static uint64_t fence_seq;
        c->hdr.flags = htole32(GPU_FLAG_FENCE | GPU_FLAG_INFO_RING_IDX);    /* a context fence, ring 0 */
        c->hdr.fence_id = htole64(++fence_seq);
    }
    c->size = htole32(sb->size); c->pad = 0;
    memcpy(submitbuf + sizeof *c, (const void *)(uintptr_t)sb->buf, sb->size);
    struct gpu_hdr *resp = (void *)(cmdbuf + 2048);
    memset(resp, 0, sizeof *resp);
    long n = virtio_request(&ctrlq, submitbuf, sizeof *c + sb->size, resp, 2048);
    uint32_t type = n < 0 ? 0 : le32toh(resp->type);
    gpu_give();
    return type == RESP_OK_NODATA ? 0 : -EIO;
}

static long gpu_dev_ioctl(struct file *f, long req, uint64_t arg)
{
    struct gpu_file *g = gpu_file_of(f);
    if (!g) {
        long rc = gpu_dev_open(f);
        if (rc) return rc;
        g = gpu_file_of(f);
    }
    switch (req) {
    case GPU_IOC_INFO: {
        if (!user_ok(arg, sizeof(struct gpu_info))) return -EFAULT;
        struct gpu_info *i = (void *)(uintptr_t)arg;
        i->ctx_id = g->ctx; i->capset_id = capset_id; i->capset_version = capset_version; i->capset_size = capset_size;
        return 0;
    }
    case GPU_IOC_CREATE_RES:
        if (!user_ok(arg, sizeof(struct gpu_res_create))) return -EFAULT;
        return gpu_create_res(g, (void *)(uintptr_t)arg);
    case GPU_IOC_DESTROY_RES: {
        if (!user_ok(arg, 4)) return -EFAULT;
        uint32_t id = *(uint32_t *)(uintptr_t)arg;
        for (struct gpu_res **pp = &g->res; *pp; pp = &(*pp)->next)
            if ((*pp)->id == id) { struct gpu_res *r = *pp; *pp = r->next; res_destroy(g, r); return 0; }
        return -ENOENT;
    }
    case GPU_IOC_TRANSFER_TO_HOST:
    case GPU_IOC_TRANSFER_FROM_HOST:
        if (!user_ok(arg, sizeof(struct gpu_transfer))) return -EFAULT;
        return gpu_transfer(g, (const void *)(uintptr_t)arg, req == GPU_IOC_TRANSFER_TO_HOST);
    case GPU_IOC_SUBMIT:
        if (!user_ok(arg, sizeof(struct gpu_submit))) return -EFAULT;
        return gpu_submit(g, (const void *)(uintptr_t)arg);
    case GPU_IOC_ATTACH_RES:
    case GPU_IOC_DETACH_RES: {
        if (!user_ok(arg, 4)) return -EFAULT;
        uint32_t id = *(uint32_t *)(uintptr_t)arg;
        return req == GPU_IOC_ATTACH_RES ? res_attach(g, id) : res_detach(g, id);
    }
    case GPU_IOC_SET_SCANOUT:
        if (!user_ok(arg, sizeof(struct gpu_scanout))) return -EFAULT;
        return gpu_set_scanout(g, (const void *)(uintptr_t)arg);
    case GPU_IOC_FLUSH: {
        if (!user_ok(arg, sizeof(struct gpu_scanout))) return -EFAULT;
        const struct gpu_scanout *so = (const void *)(uintptr_t)arg;
        if (hw_owner != g || so->res != hw_res) return -EPERM;
        gpu_take();
        flush_locked(so->res, so->x, so->y, so->w, so->h);
        gpu_give();
        return 0;
    }
    case GPU_IOC_GET_CAPSET: {
        if (!user_ok(arg, sizeof(struct gpu_capset))) return -EFAULT;
        const struct gpu_capset *cs = (const void *)(uintptr_t)arg;
        if (cs->size < capset_size || !user_ok(cs->buf, capset_size) || capset_size > 2048 - sizeof(struct gpu_hdr)) return -EINVAL;
        gpu_take();
        struct gpu_get_capset *c = (void *)cmdbuf;
        hdr(c, CMD_GET_CAPSET);
        c->capset_id = htole32(capset_id); c->capset_version = htole32(capset_version);
        uint32_t rr = cmd(sizeof *c);
        if (rr == RESP_OK_CAPSET)
            memcpy((void *)(uintptr_t)cs->buf, cmdbuf + 2048 + sizeof(struct gpu_hdr), capset_size);
        gpu_give();
        return rr == RESP_OK_CAPSET ? 0 : -EIO;
    }
    default:
        return -ENOTTY;
    }
}

/* mmap: offset = resource id * PAGE_SIZE selects the resource; its backing is mapped cached. */
static int gpu_dev_mmap(struct file *f, uint64_t virt, size_t pages, uint64_t off, int prot)
{
    (void)prot;
    struct gpu_file *g = gpu_file_of(f);
    if (!g) return -EINVAL;
    struct gpu_res *r = res_find(g, (uint32_t)(off / PAGE_SIZE));
    if (!r || pages > r->pages) return -EINVAL;
    struct task *t = task_current();
    for (size_t i = 0; i < pages; i++)
        if (vmm_map_user_page(t->mm->pgd, virt + i * PAGE_SIZE, r->phys + i * PAGE_SIZE, PTE_WRITE | PTE_DEV) != 0)
            return -ENOMEM;
    return 0;
}

const struct dev_ops gpu_dev_ops = { .ioctl = gpu_dev_ioctl, .mmap = gpu_dev_mmap, .release = gpu_dev_release };
