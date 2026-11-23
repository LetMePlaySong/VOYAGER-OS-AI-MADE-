/* Framebuffer rendering on top of the Limine-provided linear framebuffer */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "k.h"
#include "fb.h"
#include "../lib/limine.h"
#include "../lib/font8x8_basic.h"

static struct limine_framebuffer *g_fb;
static uint32_t *g_fb_addr;   /* active draw target (back buffer when present) */
static uint32_t *g_front;     /* real Limine framebuffer */
static uint32_t *g_back;      /* offscreen render buffer */
static uint32_t g_w, g_h;
static uint32_t g_pitch;      /* draw-target pitch, bytes per row */
static uint32_t g_front_pitch;
static uint32_t g_back_pitch;
static bool g_have_back;

void fb_init(void) {
    g_fb = limine_get_framebuffer();
    if (!g_fb) { kprint("FATAL: no framebuffer"); panic("no fb"); }
    g_w = (uint32_t)g_fb->width;
    g_h = (uint32_t)g_fb->height;
    g_front = (uint32_t *)g_fb->address;
    g_front_pitch = (uint32_t)g_fb->pitch;
    if (!g_have_back) {
        g_back_pitch = g_w * 4;
        g_back = (uint32_t *)kmalloc((size_t)g_w * g_h * 4);
        g_have_back = true;
    }
    /* draw into the back buffer; fb_present() flips it to the screen */
    if (g_back) { g_fb_addr = g_back; g_pitch = g_back_pitch; }
    else        { g_fb_addr = g_front; g_pitch = g_front_pitch; }
    kprintf("Framebuffer: %ux%u %u bpp pitch=%u model=%u",
            g_w, g_h, g_fb->bpp, g_front_pitch, g_fb->memory_model);
}

/* Copy the finished back buffer to the visible framebuffer in one pass.
 * Called once per rendered frame so the screen never shows a half-drawn
 * frame (no flicker/tearing). */
void fb_present(void) {
    if (!g_back || !g_front) return;
    const uint8_t *src = (const uint8_t *)g_back;
    uint8_t *dst = (uint8_t *)g_front;
    uint32_t rowbytes = g_w * 4;
    for (uint32_t y = 0; y < g_h; y++)
        memcpy(dst + (size_t)y * g_front_pitch,
               src + (size_t)y * g_back_pitch, rowbytes);
}

bool fb_ready(void) { return g_fb != NULL; }
uint32_t fb_width(void) { return g_w; }
uint32_t fb_height(void) { return g_h; }

void fb_get_target(uint32_t **addr, uint32_t *pitch_bytes) {
    if (addr) *addr = g_fb_addr;
    if (pitch_bytes) *pitch_bytes = g_pitch;
}

void fb_set_target(uint32_t *addr, uint32_t pitch_bytes) {
    g_fb_addr = addr;
    g_pitch = pitch_bytes;
}

/* temporarily treat a smaller buffer as the drawable surface: every fb
 * clip check uses g_w/g_h, so bounding them here guarantees nothing can
 * write past the end of an offscreen target (e.g. the window spin-out
 * snapshot, which is only window-sized, not screen-sized) */
void fb_set_clip(uint32_t w, uint32_t h) { g_w = w; g_h = h; }

void fb_blit_u32(const uint32_t *img) {
    uint8_t *row = (uint8_t *)g_fb_addr;
    for (uint32_t y = 0; y < g_h; y++) {
        memcpy(row, img + (size_t)y * g_w, (size_t)g_w * 4);
        row += g_pitch;
    }
}

/* Q8 sine table for 0..90 degrees (generated offline, no libm) */
static const int16_t SIN90[91] = {
    0,4,9,13,18,22,27,31,36,40,44,49,53,58,62,66,71,75,79,83,88,92,96,100,
    104,108,112,116,120,124,128,132,136,139,143,147,150,154,158,161,165,
    168,171,175,178,181,184,187,190,193,196,199,202,204,207,210,212,215,
    217,219,222,224,226,228,230,232,234,236,237,239,241,242,243,245,246,
    247,248,249,250,251,252,253,254,254,255,255,255,256,256,256,256
};

/* cos/sin in Q8 for any degree angle, via quadrant symmetry */
static void rot_cs(int deg, int *c, int *s) {
    deg %= 360; if (deg < 0) deg += 360;
    int q = deg / 90, r = deg % 90;
    int sv = SIN90[r], cv = SIN90[90 - r];
    switch (q & 3) {
        case 0:  *c = cv;  *s = sv;  break;
        case 1:  *c = -sv; *s = cv;  break;
        case 2:  *c = -cv; *s = -sv; break;
        default: *c = sv;  *s = -cv; break;
    }
}

/* spinning blit of a XRGB u32 snapshot: rotates the nw x nh window image
 * about its center by angle_deg. The destination rect is the rotated
 * image's bounding box, so the whole window stays visible while it spins
 * (no top/bottom cutoff). Pixels mapping outside the snapshot are skipped
 * (the window dissolves into space at the corners). */
void fb_blit_spin(int32_t cx, int32_t cy, int32_t nw, int32_t nh,
                  int angle_deg,
                  const uint32_t *src, int32_t sw, int32_t sh,
                  int32_t win_w, int32_t win_h,
                  int32_t snap_ox, int32_t snap_oy) {
    if (!src || nw <= 0 || nh <= 0 || sw <= 0 || sh <= 0 ||
        win_w <= 0 || win_h <= 0) return;
    int c, s;
    rot_cs(angle_deg, &c, &s);
    int ac = c < 0 ? -c : c, as = s < 0 ? -s : s;   /* |cos|,|sin| Q8 */
    /* axis-aligned bbox of the rotated nw x nh rect */
    int bw = (nw * ac + nh * as + 127) / 256;
    int bh = (nw * as + nh * ac + 127) / 256;
    if (bw < 1) bw = 1; if (bh < 1) bh = 1;
    int x0 = cx - bw / 2, y0 = cy - bh / 2;
    for (int32_t j = 0; j < bh; j++) {
        int32_t py = y0 + j;
        if (py < 0 || (uint32_t)py >= g_h) continue;
        uint8_t *drow = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch;
        int32_t v = j - bh / 2;
        for (int32_t i = 0; i < bw; i++) {
            int32_t px = x0 + i;
            if (px < 0 || (uint32_t)px >= g_w) continue;
            int32_t u = i - bw / 2;
            /* inverse-rotate the bbox offset back into window space (Q8) */
            int64_t ru = ((int64_t)u * c + (int64_t)v * s) >> 8;
            int64_t rv = ((int64_t)v * c - (int64_t)u * s) >> 8;
            /* window-space point -> original (unshrunk) window coords */
            int64_t ox = win_w / 2 + ru * win_w / nw;
            int64_t oy = win_h / 2 + rv * win_h / nh;
            /* -> snapshot pixel coords */
            int64_t sx = ox - snap_ox, sy = oy - snap_oy;
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
            *(uint32_t *)(drow + (uint32_t)px * 4) =
                src[(size_t)sy * sw + sx];
        }
    }
}

/* banded variant of fb_blit_spin: src rows live in separate kmalloc'd
 * bands of band_rows rows each (last band may be shorter). Row sy maps
 * to bands[sy / band_rows] + (sy % band_rows) * sw. */
void fb_blit_spin_bands(int32_t cx, int32_t cy, int32_t nw, int32_t nh,
                        int angle_deg,
                        const uint32_t * const *bands, int band_rows,
                        int32_t sw, int32_t sh,
                        int32_t win_w, int32_t win_h,
                        int32_t snap_ox, int32_t snap_oy) {
    if (!bands || band_rows <= 0 || nw <= 0 || nh <= 0 || sw <= 0 || sh <= 0 ||
        win_w <= 0 || win_h <= 0) return;
    int c, s;
    rot_cs(angle_deg, &c, &s);
    int ac = c < 0 ? -c : c, as = s < 0 ? -s : s;
    int bw = (nw * ac + nh * as + 127) / 256;
    int bh = (nw * as + nh * ac + 127) / 256;
    if (bw < 1) bw = 1; if (bh < 1) bh = 1;
    int x0 = cx - bw / 2, y0 = cy - bh / 2;
    for (int32_t j = 0; j < bh; j++) {
        int32_t py = y0 + j;
        if (py < 0 || (uint32_t)py >= g_h) continue;
        uint8_t *drow = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch;
        int32_t v = j - bh / 2;
        for (int32_t i = 0; i < bw; i++) {
            int32_t px = x0 + i;
            if (px < 0 || (uint32_t)px >= g_w) continue;
            int32_t u = i - bw / 2;
            int64_t ru = ((int64_t)u * c + (int64_t)v * s) >> 8;
            int64_t rv = ((int64_t)v * c - (int64_t)u * s) >> 8;
            int64_t ox = win_w / 2 + ru * win_w / nw;
            int64_t oy = win_h / 2 + rv * win_h / nh;
            int64_t sx = ox - snap_ox, sy = oy - snap_oy;
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
            int b = (int)(sy / band_rows);
            const uint32_t *band = bands[b];
            if (!band) continue;
            int inband = (int)(sy % band_rows);
            /* last band may be shorter than band_rows */
            if (inband >= sh - (int64_t)b * band_rows) continue;
            *(uint32_t *)(drow + (uint32_t)px * 4) =
                band[(size_t)inband * sw + sx];
        }
    }
}

/* scaled blit of a XRGB u32 snapshot (window spin-out): nearest-neighbour
 * sampling, clipped to the screen, src rows are tightly packed sw pixels */
void fb_blit_scaled(int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    const uint32_t *src, int32_t sw, int32_t sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t ystep = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);
    uint32_t fy = 0;
    for (int32_t j = 0; j < dh; j++) {
        int32_t sy = (int32_t)(fy >> 16);
        if (sy >= sh) sy = sh - 1;
        fy += ystep;
        const uint32_t *srow = src + (size_t)sy * sw;
        uint32_t fx = 0;
        int32_t py = dy + j;
        if (py < 0 || (uint32_t)py >= g_h) continue;
        uint8_t *drow = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch;
        for (int32_t i = 0; i < dw; i++) {
            int32_t sx = (int32_t)(fx >> 16);
            fx += xstep;
            if (sx >= sw) sx = sw - 1;
            int32_t px = dx + i;
            if (px < 0 || (uint32_t)px >= g_w) continue;
            *(uint32_t *)(drow + (uint32_t)px * 4) = srow[sx];
        }
    }
}

/* banded variant of fb_blit_scaled: src rows live in separate kmalloc'd
 * bands of band_rows rows each (same layout as fb_blit_spin_bands) */
void fb_blit_scaled_bands(int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                          const uint32_t * const *bands, int band_rows,
                          int32_t sw, int32_t sh) {
    if (!bands || band_rows <= 0 || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t ystep = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);
    uint32_t fy = 0;
    for (int32_t j = 0; j < dh; j++) {
        int32_t sy = (int32_t)(fy >> 16);
        if (sy >= sh) sy = sh - 1;
        fy += ystep;
        int b = (int)(sy / band_rows);
        const uint32_t *band = bands[b];
        if (!band) continue;
        int inband = (int)(sy % band_rows);
        if (inband >= sh - (int64_t)b * band_rows) continue;
        const uint32_t *srow = band + (size_t)inband * sw;
        uint32_t fx = 0;
        int32_t py = dy + j;
        if (py < 0 || (uint32_t)py >= g_h) continue;
        uint8_t *drow = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch;
        for (int32_t i = 0; i < dw; i++) {
            int32_t sx = (int32_t)(fx >> 16);
            fx += xstep;
            if (sx >= sw) sx = sw - 1;
            int32_t px = dx + i;
            if (px < 0 || (uint32_t)px >= g_w) continue;
            *(uint32_t *)(drow + (uint32_t)px * 4) = srow[sx];
        }
    }
}

static inline uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b) {
    /* assume 32bpp XRGB: bits 16-23 R, 8-15 G, 0-7 B */
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return pack_pixel(r, g, b); }

uint32_t fb_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    (void)a;
    return pack_pixel(r, g, b);
}

void fb_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= g_w || (uint32_t)y >= g_h) return;
    uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)y * g_pitch;
    *(uint32_t *)(row + (uint32_t)x * 4) = color;
}

void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)g_w) w = g_w - x;
    if (y + h > (int32_t)g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)y * g_pitch + (uint32_t)x * 4;
    /* sanity: row must stay inside the framebuffer; report bad calls */
    {
        uint8_t *fb_end = (uint8_t *)g_fb_addr + (size_t)g_h * g_pitch;
        uint8_t *row_end = row + (size_t)(h - 1) * g_pitch + (size_t)w * 4;
        if (row < (uint8_t *)g_fb_addr || row_end > fb_end || row_end < row) {
            void *caller = __builtin_return_address(0);
            kprintf("fb_fill_rect BAD x=%d y=%d w=%d h=%d pitch=%u caller=%p",
                    x, y, w, h, g_pitch, caller);
            return;
        }
    }
    for (int32_t j = 0; j < h; j++) {
        uint32_t *p = (uint32_t *)row;
        for (int32_t i = 0; i < w; i++) p[i] = color;
        row += g_pitch;
    }
}

void fb_blend_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    /* color in 0xAARRGGBB */
    uint32_t a = (color >> 24) & 0xFF;
    if (a == 255) { fb_fill_rect(x, y, w, h, color & 0x00FFFFFF); return; }
    if (a == 0) return;
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)g_w) w = g_w - x;
    if (y + h > (int32_t)g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t ia = 255 - a;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            int32_t px = x + i, py = y + j;
            uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch + (uint32_t)px * 4;
            uint32_t dst = *(uint32_t *)row;
            uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            /* (v)/255 via *0x8081>>23 (exact for our range) */
            uint32_t nr = ((r * a + dr * ia) * 0x8081u) >> 23;
            uint32_t ng = ((g * a + dg * ia) * 0x8081u) >> 23;
            uint32_t nb = ((b * a + db * ia) * 0x8081u) >> 23;
            *(uint32_t *)row = (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* Alpha-blend one pixel inside a rounded rect (integer math, no SSE). */
static inline void blend_px(int32_t sx, int32_t sy, uint32_t sa,
                            uint32_t r, uint32_t g, uint32_t b) {
    if (sx < 0 || sy < 0 || (uint32_t)sx >= g_w || (uint32_t)sy >= g_h) return;
    uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)sy * g_pitch + (uint32_t)sx * 4;
    uint32_t dst = *(uint32_t *)row;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t ia = 255 - sa;
    uint32_t nr = ((r * sa + dr * ia) * 0x8081u) >> 23;
    uint32_t ng = ((g * sa + dg * ia) * 0x8081u) >> 23;
    uint32_t nb = ((b * sa + db * ia) * 0x8081u) >> 23;
    *(uint32_t *)row = (nr << 16) | (ng << 8) | nb;
}

/* Alpha-blend a rounded rectangle (0xAARRGGBB). Central cross via
 * fb_blend_rect, corners via integer quarter-circle test. */
void fb_blend_rounded(int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t rad, uint32_t color) {
    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0 || w <= 0 || h <= 0) return;
    if (a == 255) { fb_fill_rounded(x, y, w, h, rad, color & 0x00FFFFFF); return; }
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    if (rad <= 0) { fb_blend_rect(x, y, w, h, color); return; }
    /* central bands (full coverage) */
    fb_blend_rect(x + rad, y, w - 2 * rad, h, color);
    fb_blend_rect(x, y + rad, w, h - 2 * rad, color);
    /* four corners: blend pixels inside each quarter circle */
    int32_t r2 = rad * rad;
    for (int32_t j = 0; j < rad; j++) {
        for (int32_t i = 0; i < rad; i++) {
            int32_t dx = rad - i, dy = rad - j;
            if (dx * dx + dy * dy > r2) continue;
            blend_px(x + i, y + j, a, r, g, b);                 /* TL */
            blend_px(x + w - 1 - i, y + j, a, r, g, b);         /* TR */
            blend_px(x + i, y + h - 1 - j, a, r, g, b);         /* BL */
            blend_px(x + w - 1 - i, y + h - 1 - j, a, r, g, b); /* BR */
        }
    }
}

void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_fill_rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, uint32_t color) {
    if (rad <= 0) { fb_fill_rect(x, y, w, h, color); return; }
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    fb_fill_rect(x + rad, y, w - 2 * rad, h, color);
    fb_fill_rect(x, y + rad, w, h - 2 * rad, color);
    /* corners: quarter circles */
    for (int32_t j = 0; j < rad; j++) {
        for (int32_t i = 0; i < rad; i++) {
            int32_t dx = rad - i, dy = rad - j;
            /* inside circle of radius rad centered at corner */
            if (dx * dx + dy * dy <= rad * rad) {
                fb_put_pixel(x + i, y + j, color);
                fb_put_pixel(x + w - 1 - i, y + j, color);
                fb_put_pixel(x + i, y + h - 1 - j, color);
                fb_put_pixel(x + w - 1 - i, y + h - 1 - j, color);
            }
        }
    }
}

void fb_draw_image(int32_t x, int32_t y, int32_t w, int32_t h,
                   const uint8_t *rgba, int32_t src_w, int32_t src_h) {
    /* draw src image at natural size offset (x,y); clip */
    (void)w; (void)h;
    for (int32_t j = 0; j < src_h; j++) {
        for (int32_t i = 0; i < src_w; i++) {
            const uint8_t *p = rgba + ((size_t)j * src_w + i) * 4;
            uint32_t a = p[3];
            uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            int32_t px = x + i, py = y + j;
            if (px < 0 || py < 0 || (uint32_t)px >= g_w || (uint32_t)py >= g_h) continue;
            uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch + (uint32_t)px * 4;
            if (a == 255) { *(uint32_t *)row = c; continue; }
            if (a == 0) continue;
            uint32_t dst = *(uint32_t *)row;
            uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            uint32_t nr = (p[0] * a + dr * (255 - a)) / 255;
            uint32_t ng = (p[1] * a + dg * (255 - a)) / 255;
            uint32_t nb = (p[2] * a + db * (255 - a)) / 255;
            *(uint32_t *)row = (nr << 16) | (ng << 8) | nb;
        }
    }
}

void fb_draw_image_scaled(int32_t x, int32_t y, int32_t w, int32_t h,
                          const uint8_t *rgba, int32_t src_w, int32_t src_h) {
    if (w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0) return;
    /* fixed-point 16.16 source stepping: avoids a divide per pixel */
    uint32_t ystep = (uint32_t)(((uint64_t)src_h << 16) / (uint32_t)h);
    uint32_t xstep = (uint32_t)(((uint64_t)src_w << 16) / (uint32_t)w);
    uint32_t fy = 0;
    for (int32_t j = 0; j < h; j++) {
        int32_t sy = (int32_t)(fy >> 16);
        if (sy >= src_h) sy = src_h - 1;
        fy += ystep;
        const uint8_t *srow = rgba + (size_t)sy * src_w * 4;
        uint32_t fx = 0;
        for (int32_t i = 0; i < w; i++) {
            int32_t sx = (int32_t)(fx >> 16);
            fx += xstep;
            if (sx >= src_w) sx = src_w - 1;
            const uint8_t *p = srow + (size_t)sx * 4;
            int32_t px = x + i, py = y + j;
            if (px < 0 || py < 0 || (uint32_t)px >= g_w || (uint32_t)py >= g_h) continue;
            uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch + (uint32_t)px * 4;
            uint32_t a = p[3];
            if (a == 0) continue;
            uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            if (a == 255) { *(uint32_t *)row = c; continue; }
            uint32_t dst = *(uint32_t *)row;
            uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            uint32_t nr = (p[0] * a + dr * (255 - a)) / 255;
            uint32_t ng = (p[1] * a + dg * (255 - a)) / 255;
            uint32_t nb = (p[2] * a + db * (255 - a)) / 255;
            *(uint32_t *)row = (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* ---------------- text ---------------- */

void fb_char(int32_t x, int32_t y, char c, uint32_t color, int scale) {
    /* font table covers all 256 byte values - high bytes select the
     * Latin-1 glyphs (degree, plusminus, ...) added to font8x8_basic */
    const char *glyph = font8x8_basic[(uint8_t)c];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = (uint8_t)glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_put_pixel(x + col * scale + sx, y + row * scale + sy, color);
            }
        }
    }
}

int fb_text(int32_t x, int32_t y, const char *s, uint32_t color, int scale) {
    int cx = x;
    int n = 0;
    while (*s) {
        if (++n > 1024) {
            void *caller = __builtin_return_address(0);
            kprintf("fb_text RUNAWAY s=%p caller=%p", s, caller);
            return cx - x;
        }
        fb_char(cx, y, *s++, color, scale);
        cx += 8 * scale + scale; /* advance + 1px spacing scaled */
    }
    /* report ink width (no trailing advance) to match fb_text_width */
    int adv = cx - x;
    return adv > 0 ? adv - scale : 0;
}

int fb_text_clip(int32_t x, int32_t y, const char *s, uint32_t color, int scale,
                 int32_t cx0, int32_t cy0, int32_t cw, int32_t ch) {
    /* clip-aware text: draw per-pixel only inside clip rect.
       Simpler: draw each char fully if its cell intersects clip. */
    int cx = x;
    int step = 8 * scale + scale;
    int ch_h = 8 * scale;
    for (const char *p = s; *p; p++) {
        /* signed char: printable ASCII (32-126) or high bytes (Latin-1
         * glyphs like degree sign) - DEL 127 and controls skipped */
        if ((*p >= 32 && *p < 127) || (uint8_t)*p >= 128) {
            /* check intersection */
            if (cx + step > cx0 && cx < cx0 + cw && y + ch_h > cy0 && y < cy0 + ch) {
                /* draw char pixels clipped */
                if ((uint8_t)*p < 128) {
                    const char *glyph = font8x8_basic[(uint8_t)*p];
                    for (int row = 0; row < 8; row++) {
                        uint8_t bits = (uint8_t)glyph[row];
                        for (int col = 0; col < 8; col++) {
                            if (!(bits & (1 << col))) continue;
                            for (int sy = 0; sy < scale; sy++) {
                                int32_t py = y + row * scale + sy;
                                if (py < cy0 || py >= cy0 + ch) continue;
                                for (int sx = 0; sx < scale; sx++) {
                                    int32_t px = cx + col * scale + sx;
                                    if (px < cx0 || px >= cx0 + cw) continue;
                                    fb_put_pixel(px, py, color);
                                }
                            }
                        }
                    }
                }
            }
        }
        cx += step;
    }
    int adv = cx - x;
    return adv > 0 ? adv - scale : 0;
}

int fb_text_width(const char *s, int scale) {
    int n = 0;
    /* guard: a runaway string means `s` is not a real C string (bad ptr).
       cap the scan and report the caller so we can find the bug. */
    while (*s) {
        if (++n > 1024) {
            void *caller = __builtin_return_address(0);
            kprintf("fb_text_width RUNAWAY s=%p caller=%p", s, caller);
            return 1024 * (8 * scale + scale);
        }
        s++;
    }
    /* ink width: n glyphs of 8*scale joined by (n-1) spacings of scale.
       Excludes the trailing advance so centered text lands true-center. */
    if (n == 0) return 0;
    return n * 8 * scale + (n - 1) * scale;
}

/* ---------------- zoom-capable text ----------------
 * fb_text_*_q render the 8x8 font at an ARBITRARY pixel height h
 * (nearest-neighbour), so the browser can do real zoom instead of
 * snapping to the integer scales 1..3. h is the on-screen glyph
 * height in pixels; advance per char = h + h/8 (glyph cell + spacing). */

static int fb_q_step(int h) { int st = h + h / 8; return st > h ? st : h + 1; }

/* alpha-blend one pixel at coverage a (0..255) - same math as
 * fb_blend_rect's inner loop */
static inline void fb_blend_px(int32_t px, int32_t py, uint32_t color, uint32_t a) {
    if (px < 0 || py < 0 || (uint32_t)px >= g_w || (uint32_t)py >= g_h) return;
    if (!a) return;
    uint8_t *row = (uint8_t *)g_fb_addr + (uint32_t)py * g_pitch + (uint32_t)px * 4;
    uint32_t dst = *(uint32_t *)row;
    if (a == 255) { *(uint32_t *)row = color & 0x00FFFFFF; return; }
    uint32_t ia = 255 - a;
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t nr = ((r * a + dr * ia) * 0x8081u) >> 23;
    uint32_t ng = ((g * a + dg * ia) * 0x8081u) >> 23;
    uint32_t nb = ((b * a + db * ia) * 0x8081u) >> 23;
    *(uint32_t *)row = (nr << 16) | (ng << 8) | nb;
}

/* Anti-aliased glyph: each destination pixel's coverage of the 8x8
 * bitmap is computed EXACTLY (area overlap in 1/h-source-pixel units,
 * footprint always 8x8 units) and alpha-blended onto the framebuffer.
 * This removes the uneven stroke widths nearest-neighbour produced at
 * non-integer heights - text looks properly smooth at every zoom. */
static void fb_char_q(int32_t x, int32_t y, char c, uint32_t color, int h) {
    const char *glyph = font8x8_basic[(uint8_t)c];
    if (h > 64) {   /* absurd sizes: keep the cheap nearest path */
        for (int py = 0; py < h; py++) {
            int row = py * 8 / h;
            uint8_t bits = (uint8_t)glyph[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) continue;
                int x0 = x + col * h / 8, x1 = x + (col + 1) * h / 8;
                for (int px = x0; px < x1; px++)
                    fb_put_pixel(px, y + py, color);
            }
        }
        return;
    }
    /* per dest-column: overlapped src cols + x-overlap (units of 1/h px,
     * footprint width always 8). Same for rows. Max cells per axis is
     * ceil(8/h)+1 <= 9. */
    int cn[64][9], cox[64][9], ccnt[64];
    int rn[64][9], roy[64][9], rcnt[64];
    for (int dx = 0; dx < h; dx++) {
        int x0 = dx * 8, x1 = (dx + 1) * 8;      /* footprint, 1/h-px units */
        int n = 0;
        int c = x0 / h;
        for (; c * h < x1 && n < 9; c++) {
            int lo = x0 > c * h ? x0 : c * h;
            int hi = x1 < (c + 1) * h ? x1 : (c + 1) * h;
            if (hi <= lo) continue;
            cn[dx][n] = c; cox[dx][n] = hi - lo; n++;
        }
        ccnt[dx] = n;
    }
    for (int dy = 0; dy < h; dy++) {
        int y0 = dy * 8, y1 = (dy + 1) * 8;
        int n = 0;
        int r = y0 / h;
        for (; r * h < y1 && n < 9; r++) {
            int lo = y0 > r * h ? y0 : r * h;
            int hi = y1 < (r + 1) * h ? y1 : (r + 1) * h;
            if (hi <= lo) continue;
            rn[dy][n] = r; roy[dy][n] = hi - lo; n++;
        }
        rcnt[dy] = n;
    }
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || (uint32_t)py >= g_h) continue;
        for (int dx = 0; dx < h; dx++) {
            int px = x + dx;
            if (px < 0 || (uint32_t)px >= g_w) continue;
            int cov = 0;
            for (int i = 0; i < ccnt[dx]; i++) {
                int cb = cn[dx][i], ox = cox[dx][i];
                int s = 0;
                for (int j = 0; j < rcnt[dy]; j++)
                    if (glyph[rn[dy][j]] & (1 << cb)) s += roy[dy][j];
                cov += ox * s;
            }
            /* cov max = 8*8 = 64 (footprint units^2) */
            uint32_t a = (uint32_t)((cov * 255 + 32) / 64);
            if (a >= 255) fb_put_pixel(px, py, color);
            else if (a) fb_blend_px(px, py, color, a);
        }
    }
}

int fb_text_q(int32_t x, int32_t y, const char *s, uint32_t color, int h) {
    if (h < 4) h = 4;
    int step = fb_q_step(h);
    int cx = x, n = 0;
    while (*s) {
        if (++n > 1024) return cx - x;
        fb_char_q(cx, y, *s++, color, h);
        cx += step;
    }
    int adv = cx - x;
    return adv > 0 ? adv - step : 0;
}

int fb_text_clip_q(int32_t x, int32_t y, const char *s, uint32_t color, int h,
                   int32_t cx0, int32_t cy0, int32_t cw, int32_t ch) {
    if (h < 4) h = 4;
    int step = fb_q_step(h);
    int cx = x, n = 0;
    for (const char *p = s; *p && n < 1024; p++, n++) {
        char c = *p;
        if ((c >= 32 && c < 127 || (uint8_t)c >= 128) &&
            cx + step > cx0 && cx < cx0 + cw && y + h > cy0 && y < cy0 + ch)
            fb_char_q(cx, y, c, color, h);
        cx += step;
    }
    int adv = cx - x;
    return adv > 0 ? adv - step : 0;
}

/* ---------------- mouse cursor ---------------- */

void fb_draw_cursor(int32_t x, int32_t y) {
    /* classic arrow cursor, 11x17-ish */
    static const uint8_t cur[17][11] = {
        {1,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,2,2,2,1},
        {1,2,2,2,2,2,2,2,2,1,1},
        {1,2,2,2,2,2,1,1,1,1,1},
        {1,2,2,1,1,1,1,1,1,1,1},
        {1,2,1,1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1,1,1,1},
    };
    uint32_t white = 0xFFFFFF, black = 0x000000;
    for (int j = 0; j < 17; j++)
        for (int i = 0; i < 11; i++) {
            if (cur[j][i] == 1) fb_put_pixel(x + i, y + j, black);
            else if (cur[j][i] == 2) fb_put_pixel(x + i, y + j, white);
        }
}
