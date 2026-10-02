/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The 3D engine of R300-class IGPs (Radeon Xpress 200M/1100/1150), shared
 * between processes through /dev/radeongpu (include/abi/radeon.h).
 *
 * The command processor is started as Linux's rs400_startup and
 * r100_cp_init do, with AMD's microcode from /lib/firmware; its ring (64
 * KiB) sits in the IGP's memory past the console's frame buffer, and a
 * scratch register counts fences. Programs get buffers in the rest of that
 * memory and submit command streams, which are checked before they reach
 * the ring: PACKET0 writes to the 3D registers only (VAP, GA, SU, SC, RS,
 * US, FG, RB3D, ZB, TX), PACKET3 only immediate-mode draws and NOPs, and
 * every colour, depth and texture address inside a buffer the program
 * owns or opened. Each submission ends with a fence. Programs never touch
 * the registers themselves.
 *
 * Built into the kernel with CONFIG_RADEON (radeon_init starts it once the
 * panel has its mode and the initrd, with the firmware, is there). */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "abi/abi.h"
#include "drivers/pci.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "asm/timer.h"
#include "spinlock.h"
#include "abi/radeon.h"
#include "endian.h"

/* registers */
#define CLOCK_CNTL_INDEX    0x0008
#define CLOCK_CNTL_DATA     0x000C
#define BUS_CNTL            0x0030
#define MC_FB_LOCATION      0x0148
#define NB_TOM              0x015C
#define CRTC_OFFSET         0x0224
#define CRTC_OFFSET_CNTL    0x0228
#define CRTC_PITCH          0x022C
#define CRTC_H_TOTAL_DISP   0x0200
#define CRTC_V_TOTAL_DISP   0x0208
#define CP_RB_BASE          0x0700
#define CP_RB_CNTL          0x0704
#define CP_RB_RPTR          0x0710
#define CP_RB_WPTR          0x0714
#define CP_RB_WPTR_DELAY    0x0718
#define CP_RB_RPTR_WR       0x071C
#define CP_CSQ_CNTL         0x0740
#define CP_CSQ_MODE         0x0744
#define SCRATCH_UMSK        0x0770
#define CP_ME_RAM_ADDR      0x07D4
#define CP_ME_RAM_DATAH     0x07DC
#define CP_ME_RAM_DATAL     0x07E0
#define CP_CSQ_STAT         0x07F8
#define RBBM_STATUS         0x0E40
#define SCRATCH_REG0        0x15E0
#define DST_PIPE_CONFIG     0x170C
#define WAIT_UNTIL          0x1720
#define ISYNC_CNTL          0x1724
#define RB2D_DSTCACHE_MODE  0x3428
#define GB_TILE_CONFIG      0x4018
#define TX_OFFSET_0         0x4540
#define RB3D_COLOROFFSET0   0x4E28
#define RB3D_DSTCACHE_CTLSTAT 0x4E4C
#define RB3D_AARESOLVE_OFFSET 0x4E80
#define ZB_DEPTHOFFSET      0x4F20
#define ZB_ZCACHE_CTLSTAT   0x4F18

#define RING_OFFSET  (8u << 20)             /* past the console (1280x800x4 = 4 MB) */
#define RING_DWORDS  (64u * 1024 / 4)
#define BO_FIRST     (16u << 20)
#define MAX_BOS      512
#define MAX_SUBMIT   (256u * 1024)          /* dwords */

struct bo {
    uint32_t handle, offset, size;
    int refs;                               /* the files that own or opened it */
    uint32_t last_fence;                    /* freeing waits for this */
};

struct cp_file {
    uint32_t handles[256];                  /* what this file may use */
    int nhandles;
    uint32_t *cmds;                         /* a kernel copy of the submission */
};

static struct {
    int ok;
    uint16_t chip;
    volatile uint32_t *regs;
    uint64_t vram_phys;
    uint32_t vram_size, vram_mc;
    volatile uint8_t *vram;                 /* the ring's part, mapped */
    volatile uint32_t *ring;
    uint32_t wptr;
    uint32_t fence;                         /* last emitted */
    struct bo bos[MAX_BOS];
    uint32_t next_handle;
    struct { uint32_t off, size; } free[256];
    int nfree;
    volatile int busy;                      /* the ring in use (sleeping lock) */
    struct cp_file *scanout_owner;
    uint32_t crtc_offset, crtc_pitch;       /* the console's */
    uint32_t width, height;
} cp;

static inline uint32_t rd(uint32_t r) { return mmio_read32((const volatile uint8_t *)cp.regs + r); }
static inline void wr(uint32_t r, uint32_t v) { mmio_write32((volatile uint8_t *)cp.regs + r, v); }
static uint32_t pll_rd(uint32_t i) { wr(CLOCK_CNTL_INDEX, i & 0x3F); return rd(CLOCK_CNTL_DATA); }
static void pll_wr(uint32_t i, uint32_t v) { wr(CLOCK_CNTL_INDEX, (i & 0x3F) | 0x80); wr(CLOCK_CNTL_DATA, v); wr(CLOCK_CNTL_INDEX, 0); }

static int wait_idle(void)
{
    for (int i = 0; i < 1000000; i++) {
        uint32_t s = rd(RBBM_STATUS);
        if ((s & 0x7F) >= 64 && !(s & (1u << 31))) return 0;
    }
    return -1;
}

static void lock(void)
{
    for (;;) {
        if (!__atomic_exchange_n(&cp.busy, 1, __ATOMIC_ACQUIRE)) return;
        task_sleep_ms(1);
    }
}
static void unlock(void) { __atomic_store_n(&cp.busy, 0, __ATOMIC_RELEASE); }

/* ---- the ring --------------------------------------------------------------------- */

static void ring_put(uint32_t v)
{
    uint32_t next = (cp.wptr + 1) & (RING_DWORDS - 1);
    if (next == rd(CP_RB_RPTR)) {                   /* full: let the CP catch up */
        wr(CP_RB_WPTR, cp.wptr);
        uint64_t t0 = timer_ms();
        while (next == rd(CP_RB_RPTR))
            if (timer_ms() - t0 > 2000) { kprintf("radeongpu: the command processor is stuck (rptr %u, RBBM_STATUS %x)\n", rd(CP_RB_RPTR), rd(RBBM_STATUS)); t0 = timer_ms(); }
    }
    cp.ring[cp.wptr] = v;
    cp.wptr = next;
}

static void ring_commit(void)
{
    while (cp.wptr & 15) { cp.ring[cp.wptr] = 0x80000000u; cp.wptr = (cp.wptr + 1) & (RING_DWORDS - 1); }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)cp.ring[(cp.wptr - 1) & (RING_DWORDS - 1)];
    wr(CP_RB_WPTR, cp.wptr);
    (void)rd(CP_RB_WPTR);
}

static uint32_t emit_fence(void)
{
    cp.fence++;
    ring_put(WAIT_UNTIL >> 2); ring_put(1u << 16 | 1u << 17 | 1u << 9);   /* 2D, 3D idle and clean */
    ring_put(SCRATCH_REG0 >> 2); ring_put(cp.fence);
    ring_commit();
    return cp.fence;
}

static uint32_t fence_done(void) { return rd(SCRATCH_REG0); }

static int fence_wait(uint32_t f, uint32_t timeout_ms)
{
    uint64_t t0 = timer_ms();
    int spins = 0;
    while ((int32_t)(fence_done() - f) < 0) {
        if (timer_ms() - t0 > timeout_ms) {
            kprintf("radeongpu: fence %u timed out (done %u, rptr %u wptr %u, RBBM_STATUS %x)\n", f, fence_done(), rd(CP_RB_RPTR), cp.wptr, rd(RBBM_STATUS));
            return -ETIMEDOUT;
        }
        if (++spins > 200 && sched_started()) task_sleep_ms(1);   /* a short wait spins, a long one sleeps */
    }
    return 0;
}

/* ---- buffers ------------------------------------------------------------------------ */

static uint32_t vram_alloc(uint32_t size)
{
    for (int i = 0; i < cp.nfree; i++)
        if (cp.free[i].size >= size) {
            uint32_t off = cp.free[i].off;
            cp.free[i].off += size;
            cp.free[i].size -= size;
            if (!cp.free[i].size) cp.free[i] = cp.free[--cp.nfree];
            return off;
        }
    return 0;
}

static void vram_free(uint32_t off, uint32_t size)
{
    for (int i = 0; i < cp.nfree; i++) {
        if (cp.free[i].off + cp.free[i].size == off) {
            cp.free[i].size += size;
            for (int j = 0; j < cp.nfree; j++)      /* and the range after, if free too */
                if (j != i && cp.free[j].off == cp.free[i].off + cp.free[i].size) { cp.free[i].size += cp.free[j].size; cp.free[j] = cp.free[--cp.nfree]; break; }
            return;
        }
        if (off + size == cp.free[i].off) { cp.free[i].off = off; cp.free[i].size += size; return; }
    }
    if (cp.nfree < 256) { cp.free[cp.nfree].off = off; cp.free[cp.nfree].size = size; cp.nfree++; }
}

static struct bo *bo_get(uint32_t handle)
{
    for (int i = 0; i < MAX_BOS; i++) if (cp.bos[i].handle == handle && handle) return &cp.bos[i];
    return NULL;
}

static int file_has(struct cp_file *f, uint32_t handle)
{
    for (int i = 0; i < f->nhandles; i++) if (f->handles[i] == handle) return 1;
    return 0;
}

static void bo_unref(struct bo *b)
{
    if (--b->refs > 0) return;
    fence_wait(b->last_fence, 2000);                /* the GPU may still read or write it */
    vram_free(b->offset, b->size);
    memset(b, 0, sizeof *b);
}

static void file_drop(struct cp_file *f, uint32_t handle)
{
    for (int i = 0; i < f->nhandles; i++)
        if (f->handles[i] == handle) {
            f->handles[i] = f->handles[--f->nhandles];
            struct bo *b = bo_get(handle);
            if (b) bo_unref(b);
            return;
        }
}

/* ---- the checker ---------------------------------------------------------------------- */

/* The registers a submission may write: the 3D engine's state. */
static int reg_allowed(uint32_t r)
{
    if (r == WAIT_UNTIL) return 1;
    if (r >= 0x2080 && r < 0x2300) return 1;        /* VAP (no PVS: this chip has none) */
    if (r == GB_TILE_CONFIG) return 0;              /* the kernel's */
    if (r >= 0x4000 && r < 0x5000) return 1;        /* GB GA SU SC RS US FG RB3D ZB TX */
    return 0;
}

/* An address the GPU will read or write: inside one of the file's buffers. */
static int addr_ok(struct cp_file *f, uint32_t mc, uint32_t fence)
{
    if (mc < cp.vram_mc) return 0;
    uint32_t off = mc - cp.vram_mc;
    for (int i = 0; i < f->nhandles; i++) {
        struct bo *b = bo_get(f->handles[i]);
        if (b && off >= b->offset && off < b->offset + b->size) { b->last_fence = fence; return 1; }
    }
    return 0;
}

static int check_reg(struct cp_file *f, uint32_t reg, uint32_t v, uint32_t fence)
{
    if (!reg_allowed(reg)) return -1;
    if ((reg >= RB3D_COLOROFFSET0 && reg < RB3D_COLOROFFSET0 + 16) || reg == ZB_DEPTHOFFSET)
        return addr_ok(f, v, fence) ? 0 : -1;
    if (reg >= TX_OFFSET_0 && reg < TX_OFFSET_0 + 64) return addr_ok(f, v & ~0x1Fu, fence) ? 0 : -1;
    if (reg == RB3D_AARESOLVE_OFFSET) return -1;
    return 0;
}

/* Walk the stream; -EINVAL on the first thing not allowed. */
static int check(struct cp_file *f, const uint32_t *c, uint32_t n, uint32_t fence)
{
    for (uint32_t i = 0; i < n;) {
        uint32_t h = c[i], type = h >> 30, count = ((h >> 16) & 0x3FFF) + 1;
        if (type == 2) { i++; continue; }
        if (i + 1 + count > n) goto bad;
        if (type == 0) {
            uint32_t reg = (h & 0x1FFF) << 2;
            int one = (h >> 15) & 1;
            for (uint32_t k = 0; k < count; k++)
                if (check_reg(f, one ? reg : reg + 4 * k, c[i + 1 + k], fence)) {
                    kprintf("radeongpu: pid %d wrote register %x (%x): not allowed\n", (int)task_current()->id, one ? reg : reg + 4 * k, c[i + 1 + k]);
                    return -EINVAL;
                }
        } else if (type == 3) {
            uint32_t op = (h >> 8) & 0xFF;
            if (op != 0x35 && op != 0x10) goto bad;    /* 3D_DRAW_IMMD_2, NOP */
        } else {
            goto bad;
        }
        i += 1 + count;
    }
    return 0;
bad:
    kprintf("radeongpu: pid %d: bad packet in its command stream\n", (int)task_current()->id);
    return -EINVAL;
}

/* ---- the device ----------------------------------------------------------------------- */

static long cp_open(struct file *fl)
{
    struct cp_file *f = kzalloc(sizeof *f);
    if (!f) return -ENOMEM;
    fl->priv_gpu = f;
    return 0;
}

static void scanout_console(void)
{
    wr(CRTC_OFFSET, cp.crtc_offset);
    wr(CRTC_PITCH, cp.crtc_pitch);
    cp.scanout_owner = NULL;
}

static void cp_release(struct file *fl)
{
    struct cp_file *f = fl->priv_gpu;
    if (!f) return;
    lock();
    if (cp.scanout_owner == f) scanout_console();
    while (f->nhandles) file_drop(f, f->handles[0]);
    unlock();
    kfree(f->cmds);
    kfree(f);
    fl->priv_gpu = NULL;
}

static long cp_ioctl(struct file *fl, long req, uint64_t arg)
{
    struct cp_file *f = fl->priv_gpu;
    if (!f) return -EINVAL;
    switch (req) {
    case RADEON_IOC_INFO: {
        if (!user_ok(arg, sizeof(struct radeon_info))) return -EFAULT;
        struct radeon_info in = { cp.vram_size, cp.vram_mc, cp.width, cp.height, cp.chip, fence_done() };
        memcpy((void *)arg, &in, sizeof in);
        return 0;
    }
    case RADEON_IOC_BO_NEW: {
        if (!user_ok(arg, sizeof(struct radeon_bo))) return -EFAULT;
        struct radeon_bo *u = (void *)arg;
        uint32_t size = (u->size + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        if (!size || f->nhandles >= 256) return -EINVAL;
        lock();
        struct bo *b = NULL;
        for (int i = 0; i < MAX_BOS && !b; i++) if (!cp.bos[i].handle) b = &cp.bos[i];
        uint32_t off = b ? vram_alloc(size) : 0;
        if (!off) { unlock(); return -ENOMEM; }
        b->handle = cp.next_handle++;
        if (!cp.next_handle) cp.next_handle = 1;
        b->offset = off; b->size = size; b->refs = 1; b->last_fence = 0;
        f->handles[f->nhandles++] = b->handle;
        u->handle = b->handle; u->size = size; u->offset = off;
        unlock();
        return 0;
    }
    case RADEON_IOC_BO_FREE:
        lock();
        if (file_has(f, (uint32_t)arg)) file_drop(f, (uint32_t)arg);
        unlock();
        return 0;
    case RADEON_IOC_BO_OPEN: {
        if (!user_ok(arg, sizeof(struct radeon_bo))) return -EFAULT;
        struct radeon_bo *u = (void *)arg;
        lock();
        struct bo *b = bo_get(u->handle);
        if (!b || f->nhandles >= 256) { unlock(); return -ENOENT; }
        if (!file_has(f, b->handle)) { b->refs++; f->handles[f->nhandles++] = b->handle; }
        u->size = b->size; u->offset = b->offset;
        unlock();
        return 0;
    }
    case RADEON_IOC_SUBMIT: {
        if (!user_ok(arg, sizeof(struct radeon_submit))) return -EFAULT;
        struct radeon_submit *u = (void *)arg;
        uint32_t n = u->dwords;
        if (n > MAX_SUBMIT || !user_ok(u->cmds, (uint64_t)n * 4)) return -EINVAL;
        if (!f->cmds) f->cmds = kmalloc(MAX_SUBMIT * 4);
        if (!f->cmds) return -ENOMEM;
        memcpy(f->cmds, (const void *)u->cmds, (size_t)n * 4);     /* checked and run from our copy */
        lock();
        uint32_t fence = cp.fence + 1;
        if (check(f, f->cmds, n, fence)) { unlock(); return -EINVAL; }
        for (uint32_t i = 0; i < n; i++) ring_put(f->cmds[i]);
        u->fence = emit_fence();
        unlock();
        return 0;
    }
    case RADEON_IOC_WAIT: {
        if (!user_ok(arg, sizeof(struct radeon_wait))) return -EFAULT;
        struct radeon_wait *u = (void *)arg;
        return fence_wait(u->fence, u->timeout_ms ? u->timeout_ms : 2000);
    }
    case RADEON_IOC_SCANOUT: {
        if (!user_ok(arg, sizeof(struct radeon_scanout))) return -EFAULT;
        struct radeon_scanout *u = (void *)arg;
        lock();
        if (!u->handle) {
            if (cp.scanout_owner == f) scanout_console();
            unlock();
            return 0;
        }
        struct bo *b = file_has(f, u->handle) ? bo_get(u->handle) : NULL;
        if (!b || u->pitch < cp.width * 4 || (u->pitch & 63) || (uint64_t)u->pitch * cp.height > b->size) { unlock(); return -EINVAL; }
        /* the CRTC reads from here now; the pitch in units of 8 pixels */
        wr(CRTC_OFFSET, b->offset);
        wr(CRTC_PITCH, (u->pitch / 32) | (u->pitch / 32) << 16);
        cp.scanout_owner = f;
        unlock();
        return 0;
    }
    }
    return -EINVAL;
}

/* Buffers only, write-combining: never the registers. */
static int cp_mmap(struct file *fl, uint64_t virt, size_t pages, uint64_t off, int prot)
{
    (void)prot;
    struct cp_file *f = fl->priv_gpu;
    if (!f) return -EINVAL;
    lock();
    int ok = 0;
    for (int i = 0; i < f->nhandles && !ok; i++) {
        struct bo *b = bo_get(f->handles[i]);
        if (b && off >= b->offset && off + pages * PAGE_SIZE <= (uint64_t)b->offset + b->size) ok = 1;
    }
    unlock();
    if (!ok) return -EINVAL;
    struct task *t = task_current();
    for (size_t i = 0; i < pages; i++)
        if (vmm_map_user_page(t->mm->pgd, virt + i * PAGE_SIZE, cp.vram_phys + off + i * PAGE_SIZE, PTE_WRITE | PTE_DEV | PTE_WC) != 0)
            return -ENOMEM;
    return 0;
}

static const struct dev_ops cp_ops = { .open = cp_open, .ioctl = cp_ioctl, .mmap = cp_mmap, .release = cp_release };

/* ---- bring-up -------------------------------------------------------------------------- */

static int load_microcode(void)
{
    struct vnode *n = vfs_lookup(vfs_root(), "/lib/firmware/radeon/R300_cp.bin");
    size_t size = 0;
    uint8_t *fw = n ? vfs_read_all(n, &size) : NULL;
    if (!fw || size == 0 || size % 8 || size > 4096) {
        kprintf("radeongpu: no /lib/firmware/radeon/R300_cp.bin: no 3D\n");
        if (fw) kfree(fw);
        return -1;
    }
    wr(CP_CSQ_MODE, 0);
    wr(CP_CSQ_CNTL, 0);
    wait_idle();
    wr(CP_ME_RAM_ADDR, 0);
    for (size_t i = 0; i < size; i += 8) {
        wr(CP_ME_RAM_DATAH, (uint32_t)fw[i] << 24 | (uint32_t)fw[i + 1] << 16 | (uint32_t)fw[i + 2] << 8 | fw[i + 3]);
        wr(CP_ME_RAM_DATAL, (uint32_t)fw[i + 4] << 24 | (uint32_t)fw[i + 5] << 16 | (uint32_t)fw[i + 6] << 8 | fw[i + 7]);
    }
    kfree(fw);
    return 0;
}

int radeon_cp_start(void)
{
    const struct pci_dev *dev = NULL;
    for (size_t i = 0; i < pci_count() && !dev; i++) {
        const struct pci_dev *p = pci_get(i);
        if (p->vendor == 0x1002 && (p->device == 0x5954 || p->device == 0x5955 || p->device == 0x5974 || p->device == 0x5975)) dev = p;
    }
    if (!dev) return -1;
    memset(&cp, 0, sizeof cp);
    cp.chip = dev->device;
    cp.regs = vmm_map_mmio(dev->bar[2], 0x10000);
    cp.vram_phys = dev->bar[0];
    uint32_t tom = rd(NB_TOM);
    cp.vram_size = (((tom >> 16) - (tom & 0xFFFF) + 1) << 6) * 1024;
    cp.vram_mc = (rd(MC_FB_LOCATION) & 0xFFFF) << 16;
    cp.crtc_offset = rd(CRTC_OFFSET);
    cp.crtc_pitch = rd(CRTC_PITCH);
    cp.width = ((rd(CRTC_H_TOTAL_DISP) >> 16) + 1) * 8;
    cp.height = (rd(CRTC_V_TOTAL_DISP) >> 16) + 1;
    cp.vram = vmm_map_wc(cp.vram_phys + RING_OFFSET, RING_DWORDS * 4);
    cp.ring = (volatile uint32_t *)cp.vram;

    /* rs400_startup: clocks for the CP, one pipe, the 2D cache, bus mastering */
    pll_wr(0x0D, pll_rd(0x0D) | 1u << 16 | 1u << 23);
    wr(GB_TILE_CONFIG, 1u << 0 | 1u << 4);
    wait_idle();
    wr(DST_PIPE_CONFIG, rd(DST_PIPE_CONFIG) | 1u << 31);
    wr(RB2D_DSTCACHE_MODE, 1u << 8 | 1u << 17);
    wait_idle();
    wr(BUS_CNTL, rd(BUS_CNTL) & ~(1u << 6));
    if (load_microcode()) return -1;

    for (uint32_t i = 0; i < RING_DWORDS; i++) cp.ring[i] = 0x80000000u;
    uint32_t bufsz = 0;
    while ((1u << (bufsz + 1)) * 4 < RING_DWORDS * 4) bufsz++;
    uint32_t cntl = bufsz | 9u << 8 | 1u << 18 | 1u << 27;    /* no read-pointer write-back */
    wr(CP_RB_WPTR_DELAY, 64);
    wr(CP_RB_CNTL, cntl);
    wr(CP_RB_BASE, cp.vram_mc + RING_OFFSET);
    wr(CP_RB_CNTL, cntl | 1u << 31);
    wr(CP_RB_RPTR_WR, 0);
    cp.wptr = 0;
    wr(CP_RB_WPTR, 0);
    wr(SCRATCH_UMSK, 0);
    wr(CP_RB_CNTL, cntl);
    wr(CP_RB_WPTR_DELAY, 0);
    wr(CP_CSQ_MODE, 0x00004D4D);
    wr(CP_CSQ_CNTL, 4u << 28);
    wr(SCRATCH_REG0, 0);
    ring_put(ISYNC_CNTL >> 2); ring_put(1u << 0 | 1u << 1 | 1u << 4 | 1u << 5);
    uint32_t f = emit_fence();
    if (fence_wait(f, 500)) { kprintf("radeongpu: the command processor does not run\n"); return -1; }

    cp.free[0].off = BO_FIRST;
    cp.free[0].size = cp.vram_size - BO_FIRST;
    cp.nfree = 1;
    cp.next_handle = 1;
    cp.ok = 1;
    vfs_mkdev("/dev/radeongpu", &cp_ops, NULL);
    kprintf("radeongpu: 3D engine running, %u MiB for buffers -> /dev/radeongpu\n", (cp.vram_size - BO_FIRST) >> 20);
    return 0;
}

void radeon_cp_stop(void)
{
    if (!cp.ok) return;
    fence_wait(cp.fence, 2000);
    if (cp.scanout_owner) scanout_console();
    wr(CP_CSQ_MODE, 0);
    wr(CP_CSQ_CNTL, 0);
    vfs_unlink(vfs_root(), "/dev/radeongpu");
    cp.ok = 0;
}
