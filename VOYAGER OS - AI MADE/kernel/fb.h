/* Framebuffer drawing primitives */
#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void fb_init(void);
bool fb_ready(void);
/* flip the finished back buffer to the screen (call once per frame) */
void fb_present(void);
uint32_t fb_width(void);
uint32_t fb_height(void);

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t fb_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* direct access for fast full-screen copies (u32 XRGB pixels, row-major) */
void fb_get_target(uint32_t **addr, uint32_t *pitch_bytes);
/* redirect all drawing to an arbitrary tightly-packed/pitched buffer;
 * pass the saved get_target values to restore */
void fb_set_target(uint32_t *addr, uint32_t pitch_bytes);
void fb_set_clip(uint32_t w, uint32_t h);   /* bound draw surface (offscreen) */
void fb_blit_u32(const uint32_t *img);   /* img is w*h pixels, XRGB */
/* scaled blit of a tightly-packed XRGB u32 snapshot (spin-out close) */
void fb_blit_scaled(int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    const uint32_t *src, int32_t sw, int32_t sh);
/* banded variant: src rows live in separate band_rows-tall buffers */
void fb_blit_scaled_bands(int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                          const uint32_t * const *bands, int band_rows,
                          int32_t sw, int32_t sh);
/* spinning blit: rotates the nw x nh window snapshot about its center;
 * dst is the rotated bounding box so nothing is clipped. src is a
 * tightly-packed snapshot of the win_w x win_h window starting at
 * (snap_ox,snap_oy) inside it. */
void fb_blit_spin(int32_t cx, int32_t cy, int32_t nw, int32_t nh,
                  int angle_deg,
                  const uint32_t *src, int32_t sw, int32_t sh,
                  int32_t win_w, int32_t win_h,
                  int32_t snap_ox, int32_t snap_oy);
/* same spin blit but src is a set of horizontal bands (each band_rows
 * tall, tightly packed sw pixels wide) - used when one big snapshot
 * buffer can't be allocated contiguously */
void fb_blit_spin_bands(int32_t cx, int32_t cy, int32_t nw, int32_t nh,
                        int angle_deg,
                        const uint32_t * const *bands, int band_rows,
                        int32_t sw, int32_t sh,
                        int32_t win_w, int32_t win_h,
                        int32_t snap_ox, int32_t snap_oy);

void fb_put_pixel(int32_t x, int32_t y, uint32_t color);
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void fb_fill_rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, uint32_t color);
void fb_blend_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color); /* alpha blend */
void fb_blend_rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, uint32_t color);
void fb_draw_image(int32_t x, int32_t y, int32_t w, int32_t h,
                   const uint8_t *rgba, int32_t src_w, int32_t src_h);
void fb_draw_image_scaled(int32_t x, int32_t y, int32_t w, int32_t h,
                          const uint8_t *rgba, int32_t src_w, int32_t src_h);

/* text: 8x8 font scaled by `scale`, returns width drawn */
int fb_text(int32_t x, int32_t y, const char *s, uint32_t color, int scale);
int fb_text_clip(int32_t x, int32_t y, const char *s, uint32_t color, int scale,
                 int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h);
int fb_text_width(const char *s, int scale);
void fb_char(int32_t x, int32_t y, char c, uint32_t color, int scale);
/* zoom-capable text: glyph height h px arbitrary (not just int scales) */
int fb_text_q(int32_t x, int32_t y, const char *s, uint32_t color, int h);
int fb_text_clip_q(int32_t x, int32_t y, const char *s, uint32_t color, int h,
                   int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h);

/* cursor */
void fb_draw_cursor(int32_t x, int32_t y);

/* PNG decode: returns 0 on success, fills rgba (caller allocs w*h*4) */
int png_decode(const uint8_t *data, size_t size,
               uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h);
/* decode PNG into a static buffer region (no alloc): returns ptr or NULL */
uint8_t *png_decode_static(const uint8_t *data, size_t size,
                           uint32_t *out_w, uint32_t *out_h);


#ifdef __cplusplus
}
#endif

#endif
