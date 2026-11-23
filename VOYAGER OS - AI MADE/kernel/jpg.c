/* JPEG decode wrapper around picojpeg (public domain baseline decoder).
 * Feeds the whole buffer through the need-bytes callback, then assembles
 * each MCU's R/G/B planes into an RGBA8888 image. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "picojpeg.h"

void *kmalloc(size_t size);
void kfree(void *ptr);

typedef struct { const uint8_t *d; uint32_t len, pos; } JFeed;
static JFeed *g_jf;

static uint8_t jpg_feed_cb(uint8_t *buf, uint8_t sz, uint8_t *got, void *cd) {
    (void)cd;
    uint32_t avail = g_jf->len - g_jf->pos;
    if (avail > sz) avail = sz;
    if (avail) memcpy(buf, g_jf->d + g_jf->pos, avail);
    g_jf->pos += avail;
    *got = (uint8_t)avail;
    /* pjpeg treats the RETURN value as an error status - 0 = ok.
     * EOF is signalled by *got == 0, not by a nonzero return. */
    return 0;
}

int jpg_decode(const uint8_t *d, uint32_t n,
               uint8_t **out, int *ow, int *oh) {
    JFeed f = { d, n, 0 };
    g_jf = &f;
    pjpeg_image_info_t info;
    uint8_t rc = pjpeg_decode_init(&info, jpg_feed_cb, NULL, 0);
    if (rc) return -1;
    int w = info.m_width, h = info.m_height;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    uint8_t *rgba = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!rgba) return -1;
    int mcus_x = info.m_MCUSPerRow, mcus_y = info.m_MCUSPerCol;
    int mw = info.m_MCUWidth, mh = info.m_MCUHeight;
    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {
            if (pjpeg_decode_mcu() != 0) { kfree(rgba); return -1; }
            int bx = mx * mw, by = my * mh;
            for (int j = 0; j < mh; j++) {
                for (int i = 0; i < mw; i++) {
                    int px = bx + i, py = by + j;
                    if (px >= w || py >= h) continue;
                    /* which 8x8 block + intra-block offset */
                    int bi, bj, ofs;
                    switch (info.m_scanType) {
                    case PJPG_GRAYSCALE:
                    case PJPG_YH1V1:
                        bi = 0; bj = 0; ofs = j * 8 + i; break;
                    case PJPG_YH2V1:           /* 16x8: two side-by-side */
                        bi = i >> 3; bj = 0; ofs = j * 8 + (i & 7); break;
                    case PJPG_YH1V2:           /* 8x16: two stacked */
                        bi = 0; bj = j >> 3; ofs = (j & 7) * 8 + i; break;
                    default:                   /* YH2V2: 2x2 grid */
                        bi = i >> 3; bj = j >> 3;
                        ofs = (j & 7) * 8 + (i & 7); break;
                    }
                    static const int bo[4] = { 0, 64, 128, 192 };
                    int bo2 = bo[bj * 2 + bi];
                    const uint8_t *pr = info.m_pMCUBufR + bo2 + ofs;
                    const uint8_t *pg = info.m_pMCUBufG ? info.m_pMCUBufG + bo2 + ofs : pr;
                    const uint8_t *pb = info.m_pMCUBufB ? info.m_pMCUBufB + bo2 + ofs : pr;
                    uint8_t *o = rgba + ((size_t)py * w + px) * 4;
                    o[0] = *pr; o[1] = *pg; o[2] = *pb; o[3] = 255;
                }
            }
        }
    }
    *out = rgba; *ow = w; *oh = h;
    return 0;
}
