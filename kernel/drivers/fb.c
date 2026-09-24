/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "drivers/fb.h"
#include "string.h"
#include "asm/io.h"
#include "fs/vfs.h"
#include "abi/abi.h"
#include "abi/fb.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "proc/wait.h"

static struct zaeboot_framebuffer raw_fb;
static int kd_mode = KD_TEXT;
static struct file *kd_owner;       /* the fd that set KD_GRAPHICS */

int fb_is_graphics_mode(void)
{
    return kd_mode == KD_GRAPHICS;
}

extern const uint8_t font8x8[95][8];

#define GLYPH_W 8
#define GLYPH_H 8
#define SCALE   2
#define CELL_W  (GLYPH_W * SCALE)
#define CELL_H  (GLYPH_H * SCALE)

static struct {
    uint8_t *base;
    uint32_t width, height, pitch;
    uint8_t rs, gs, bs;
    uint32_t cols, rows;
    uint32_t cx, cy;
    uint32_t fg, bg;        /* native pixel values */
    int ready;
    int text;               /* VGA text mode at 0xB8000 (BIOS boot without a VBE mode) */
} con;

/* ---- VGA text fallback ------------------------------------------------------- */
#define VGA_TEXT ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x0700

static void vga_cursor(void)
{
    uint16_t pos = (uint16_t)(con.cy * VGA_COLS + con.cx);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)pos);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

static void vga_putc(char c)
{
    switch (c) {
    case '\n': con.cx = 0; con.cy++; break;
    case '\r': con.cx = 0; break;
    case '\t': do { VGA_TEXT[con.cy * VGA_COLS + con.cx++] = VGA_ATTR | ' '; } while (con.cx % 4 && con.cx < VGA_COLS); break;
    case '\b': if (con.cx > 0) VGA_TEXT[con.cy * VGA_COLS + --con.cx] = VGA_ATTR | ' '; break;
    default:
        VGA_TEXT[con.cy * VGA_COLS + con.cx] = VGA_ATTR | (uint8_t)c;
        con.cx++;
    }
    if (con.cx >= VGA_COLS) { con.cx = 0; con.cy++; }
    if (con.cy >= VGA_ROWS) {
        for (uint32_t i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
            VGA_TEXT[i] = VGA_TEXT[i + VGA_COLS];
        for (uint32_t i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
            VGA_TEXT[i] = VGA_ATTR | ' ';
        con.cy = VGA_ROWS - 1;
    }
    vga_cursor();
}

/* Continue where the BIOS/loader left the cursor (BDA 0x450). */
void fb_init_text(void)
{
    const uint8_t *cur = (const uint8_t *)0x450;
    con.cx = cur[0] < VGA_COLS ? cur[0] : 0;
    con.cy = cur[1] < VGA_ROWS ? cur[1] : VGA_ROWS - 1;
    con.cols = VGA_COLS;
    con.rows = VGA_ROWS;
    con.text = 1;
    con.ready = 1;
}

static uint32_t to_native(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r << con.rs) | (g << con.gs) | (b << con.bs);
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t px)
{
    *(uint32_t *)(con.base + y * con.pitch + x * 4) = px;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px)
{
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            put_pixel(x + i, y + j, px);
}

static uint8_t *fb_virt;                 /* where the kernel writes the pixels */
static int fb_in_ram;                    /* backing is ordinary memory (a virtio-gpu resource): map it cached */
static int (*present_hook)(void);        /* a display that shows the buffer only when told to */
static int (*present_rect_hook)(uint32_t, uint32_t, uint32_t, uint32_t);
static void (*damage_hook)(uint32_t, uint32_t, uint32_t, uint32_t);
#define DAMAGE(x, y, w, h) do { if (damage_hook) damage_hook(x, y, w, h); } while (0)
static uint64_t fb_map_len;              /* how much of the buffer /dev/fb0 lets a process map */
static uint32_t fb_mode_gen = 1;         /* bumped by fb_mode_change */
static struct waitqueue fb_wq = WAITQUEUE_INIT;

void fb_init(const struct zaeboot_framebuffer *fb, void *virt, int in_ram)
{
    if (fb->base == 0 || fb->bpp != 32)
        return;
    raw_fb = *fb;
    fb_virt = virt;
    fb_in_ram = in_ram;
    fb_map_len = (uint64_t)fb->pitch * fb->height;
    con.text = 0;
    con.base   = virt;
    con.width  = fb->width;
    con.height = fb->height;
    con.pitch  = fb->pitch;
    con.rs = fb->red_shift;
    con.gs = fb->green_shift;
    con.bs = fb->blue_shift;
    con.cols = con.width / CELL_W;
    con.rows = con.height / CELL_H;
    con.cx = con.cy = 0;
    con.fg = to_native(0xFFFFFF);
    con.bg = to_native(0x000000);
    con.ready = 1;
}

void fb_set_color(uint32_t fg, uint32_t bg)
{
    if (con.text)
        return;
    con.fg = to_native(fg);
    con.bg = to_native(bg);
}

void fb_clear(void)
{
    if (!con.ready || con.text)
        return;
    fill_rect(0, 0, con.width, con.height, con.bg);
    con.cx = con.cy = 0;
    DAMAGE(0, 0, con.width, con.height);
}

static void draw_glyph(uint32_t col, uint32_t row, char c)
{
    const uint8_t *glyph = (c >= 0x20 && c <= 0x7E) ? font8x8[c - 0x20] : font8x8['?' - 0x20];
    uint32_t x0 = col * CELL_W, y0 = row * CELL_H;

    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            uint32_t px = (bits >> gx) & 1 ? con.fg : con.bg;
            fill_rect(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE, px);
        }
    }
    DAMAGE(x0, y0, CELL_W, CELL_H);
}

static void scroll(void)
{
    uint32_t line_bytes = CELL_H * con.pitch;
    uint32_t used_rows = con.rows * CELL_H;
    memcpy(con.base, con.base + line_bytes, (used_rows - CELL_H) * con.pitch);
    fill_rect(0, used_rows - CELL_H, con.width, CELL_H, con.bg);
    DAMAGE(0, 0, con.width, used_rows);
}

static void newline(void)
{
    con.cx = 0;
    if (++con.cy >= con.rows) {
        scroll();
        con.cy = con.rows - 1;
    }
}

void fb_putc(char c)
{
    if (!con.ready || kd_mode == KD_GRAPHICS)
        return;
    if (con.text) {
        vga_putc(c);
        return;
    }

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        con.cx = 0;
        return;
    case '\t':
        do {
            fb_putc(' ');
        } while (con.cx % 4);
        return;
    case '\b':
        if (con.cx > 0) {
            con.cx--;
            draw_glyph(con.cx, con.cy, ' ');
        }
        return;
    }

    draw_glyph(con.cx, con.cy, c);
    if (++con.cx >= con.cols)
        newline();
}

void fb_puts(const char *s)
{
    while (*s)
        fb_putc(*s++);
}

/* ---- /dev/fb0 character device ---------------------------------------------- */

static long fb_dev_read(struct file *f, void *buf, size_t len)
{
    uint64_t size = (uint64_t)raw_fb.pitch * raw_fb.height;
    if (f->pos >= size)
        return 0;
    if (len > size - f->pos)
        len = (size_t)(size - f->pos);
    memcpy(buf, fb_virt + f->pos, len);
    f->pos += len;
    return (long)len;
}

static long fb_dev_write(struct file *f, const void *buf, size_t len)
{
    uint64_t size = (uint64_t)raw_fb.pitch * raw_fb.height;
    if (f->pos >= size)
        return 0;
    if (len > size - f->pos)
        len = (size_t)(size - f->pos);
    memcpy(fb_virt + f->pos, buf, len);
    f->pos += len;
    return (long)len;
}

void fb_mode_change(uint32_t width, uint32_t height, uint32_t pitch, uint64_t map_len)
{
    raw_fb.width = width; raw_fb.height = height; raw_fb.pitch = pitch;
    fb_map_len = map_len;
    con.width = width; con.height = height; con.pitch = pitch;
    con.cols = width / CELL_W; con.rows = height / CELL_H;
    if (con.cx >= con.cols) con.cx = 0;
    if (con.cy >= con.rows) con.cy = con.rows ? con.rows - 1 : 0;
    fb_mode_gen++;
    waitqueue_wake_all(&fb_wq);
}

static int fb_dev_poll(struct file *f, struct waitqueue **wq)
{
    *wq = &fb_wq;
    return POLLOUT | (f->fb_gen != fb_mode_gen ? POLLPRI : 0);
}

static long fb_dev_ioctl(struct file *f, long req, uint64_t arg)
{
    uint64_t size = fb_map_len ? fb_map_len : (uint64_t)raw_fb.pitch * raw_fb.height;
    switch (req) {
    case FBIOGET_FSCREENINFO: {
        if (!user_ok(arg, sizeof(struct fb_fix_screeninfo))) return -EFAULT;
        struct fb_fix_screeninfo finfo;
        memset(&finfo, 0, sizeof(finfo));
        memcpy(finfo.id, "sicfb", 6);
        finfo.smem_start = raw_fb.base;
        finfo.smem_len = (uint32_t)size;
        finfo.type = FB_TYPE_PACKED_PIXELS;
        finfo.visual = FB_VISUAL_TRUECOLOR;
        finfo.line_length = raw_fb.pitch;
        memcpy((void *)arg, &finfo, sizeof(finfo));
        return 0;
    }
    case FBIOGET_VSCREENINFO: {
        if (!user_ok(arg, sizeof(struct fb_var_screeninfo))) return -EFAULT;
        struct fb_var_screeninfo vinfo;
        memset(&vinfo, 0, sizeof(vinfo));
        vinfo.xres = raw_fb.width;
        vinfo.yres = raw_fb.height;
        vinfo.xres_virtual = raw_fb.width;
        vinfo.yres_virtual = raw_fb.height;
        vinfo.bits_per_pixel = raw_fb.bpp ? raw_fb.bpp : 32;
        vinfo.red.offset = raw_fb.red_shift;
        vinfo.red.length = 8;
        vinfo.green.offset = raw_fb.green_shift;
        vinfo.green.length = 8;
        vinfo.blue.offset = raw_fb.blue_shift;
        vinfo.blue.length = 8;
        vinfo.transp.offset = 24;
        vinfo.transp.length = 8;
        memcpy((void *)arg, &vinfo, sizeof(vinfo));
        f->fb_gen = fb_mode_gen;        /* seen: no POLLPRI until the next change */
        return 0;
    }
    case FBIOPUT_VSCREENINFO:
        if (!user_ok(arg, sizeof(struct fb_var_screeninfo))) return -EFAULT;
        return 0;
    case FBIOPRESENT:
        return present_hook ? present_hook() : 0;   /* scanout displays show writes as they happen */
    case FBIOPRESENT_RECT: {
        if (!user_ok(arg, sizeof(struct fb_rect))) return -EFAULT;
        struct fb_rect r = *(const struct fb_rect *)arg;
        if (r.x >= raw_fb.width || r.y >= raw_fb.height) return 0;
        if (r.w > raw_fb.width - r.x) r.w = raw_fb.width - r.x;
        if (r.h > raw_fb.height - r.y) r.h = raw_fb.height - r.y;
        if (present_rect_hook) return present_rect_hook(r.x, r.y, r.w, r.h);
        return present_hook ? present_hook() : 0;
    }
    case KDSETMODE:
        if (arg == KD_GRAPHICS) {
            kd_mode = KD_GRAPHICS;
            kd_owner = f;               /* text comes back when this file closes */
        } else if (arg == KD_TEXT) {
            kd_mode = KD_TEXT;
            kd_owner = NULL;
            fb_clear();                 /* the game's last frame is still there */
        } else {
            return -EINVAL;
        }
        return 0;
    case KDGETMODE:
        if (!user_ok(arg, sizeof(int))) return -EFAULT;
        *(int *)arg = kd_mode;
        return 0;
    default:
        return -ENOTTY;
    }
}

static int fb_dev_mmap(struct file *f, uint64_t virt, size_t pages, uint64_t off, int prot)
{
    (void)f; (void)prot;
    uint64_t size = PAGE_ALIGN_UP(fb_map_len ? fb_map_len : (uint64_t)raw_fb.pitch * raw_fb.height);
    if (off >= size || off + pages * PAGE_SIZE > size)
        return -EINVAL;
    struct task *t = task_current();
    for (size_t i = 0; i < pages; i++) {
        uint64_t paddr = raw_fb.base + off + i * PAGE_SIZE;
        uint64_t vaddr = virt + i * PAGE_SIZE;
        if (vmm_map_user_page(t->mm->pgd, vaddr, paddr, PTE_WRITE | PTE_DEV | (fb_in_ram ? 0 : PTE_PCD | PTE_PWT)) != 0)
            return -ENOMEM;
    }
    return 0;
}

/* The process that switched to graphics is gone (or closed the fd): give the
 * text console back so a crashed game doesn't leave a dead screen. */
static void fb_dev_release(struct file *f)
{
    if (f == kd_owner) {
        kd_owner = NULL;
        kd_mode = KD_TEXT;
        fb_clear();
    }
}

static const struct dev_ops fb_dev_ops = {
    .read  = fb_dev_read,
    .write = fb_dev_write,
    .poll  = fb_dev_poll,
    .ioctl = fb_dev_ioctl,
    .mmap  = fb_dev_mmap,
    .release = fb_dev_release,
};

void fb_set_present_hook(int (*fn)(void)) { present_hook = fn; }
void fb_set_present_rect_hook(int (*fn)(uint32_t, uint32_t, uint32_t, uint32_t)) { present_rect_hook = fn; }
void fb_set_damage_hook(void (*fn)(uint32_t, uint32_t, uint32_t, uint32_t)) { damage_hook = fn; }

void fb_dev_init(void)
{
    if (raw_fb.base == 0 || vfs_lookup(vfs_root(), "/dev/fb0"))
        return;                         /* nothing to show yet, or already there */
    vfs_mkdev("/dev/fb0", &fb_dev_ops, NULL);
    struct vnode *n = vfs_lookup(vfs_root(), "/dev/fb0");
    if (n) {
        n->size = (uint64_t)raw_fb.pitch * raw_fb.height;
        n->seekable = 1;
    }
}
