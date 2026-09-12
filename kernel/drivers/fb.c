/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "drivers/fb.h"
#include "string.h"

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
} con;

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

void fb_init(const struct zaeboot_framebuffer *fb)
{
    if (fb->base == 0 || fb->bpp != 32)
        return;
    con.base   = (uint8_t *)fb->base;
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
    con.fg = to_native(fg);
    con.bg = to_native(bg);
}

void fb_clear(void)
{
    if (!con.ready)
        return;
    fill_rect(0, 0, con.width, con.height, con.bg);
    con.cx = con.cy = 0;
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
}

static void scroll(void)
{
    uint32_t line_bytes = CELL_H * con.pitch;
    uint32_t used_rows = con.rows * CELL_H;
    memcpy(con.base, con.base + line_bytes, (used_rows - CELL_H) * con.pitch);
    fill_rect(0, used_rows - CELL_H, con.width, CELL_H, con.bg);
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
    if (!con.ready)
        return;

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
