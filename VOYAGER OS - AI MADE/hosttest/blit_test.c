#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { uint8_t *rgba; int delay_cs; } GifFrame;
int gif_decode_anim(const uint8_t *d, uint32_t n, GifFrame **out, int *on, int *ow, int *oh);

/* ---- stubs for fb ---- */
static uint32_t surf[800*600];
static uint32_t SW=800, SH=600;
uint32_t *fb_get_target(uint32_t **p, uint32_t *pitch){ *p=surf; *pitch=SW*4; return 0; }
uint32_t fb_width(void){ return SW; }
uint32_t fb_height(void){ return SH; }

/* ---- copy of blit_img from Voyage Web.cpp ---- */
static void blit_img(int gx, int gy, int dw, int dh,
                     const uint8_t *rgba, int sw, int sh,
                     int fx0, int fy0, int fw, int fh) {
    if (!rgba || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        fw <= 0 || fh <= 0) return;
    uint32_t *fb; uint32_t pitch;
    fb_get_target(&fb, &pitch);
    if (!fb || pitch < 8) return;
    int surfw = (int)fb_width(), surfh = (int)fb_height();
    if (gx < 0) { dw += gx; gx = 0; }
    if (gy < 0) { dh += gy; gy = 0; }
    if (gx + dw > surfw) dw = surfw - gx;
    if (gy + dh > surfh) dh = surfh - gy;
    if (dw <= 0 || dh <= 0) return;
    int xoff = gx - fx0, yoff = gy - fy0;
    for (int j = 0; j < dh; j++) {
        int sy = (int)(((int64_t)(yoff + j) * sh) / fh);
        if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = rgba + (size_t)sy * sw * 4;
        uint32_t *row = fb + (size_t)(gy + j) * (pitch / 4) + gx;
        for (int i = 0; i < dw; i++) {
            int sx = (int)(((int64_t)(xoff + i) * sw) / fw);
            if (sx >= sw) sx = sw - 1;
            const uint8_t *px = srow + (size_t)sx * 4;
            uint32_t a = px[3];
            if (a == 0) { row++; continue; }
            if (a == 255) {
                *row++ = 0xFF000000u | ((uint32_t)px[0] << 16) |
                         ((uint32_t)px[1] << 8) | px[2];
                continue;
            }
            uint32_t d0 = *row;
            uint32_t dr = (d0 >> 16) & 0xFF, dg = (d0 >> 8) & 0xFF, db = d0 & 0xFF;
            uint32_t r = (px[0]*a + dr*(255-a))/255;
            uint32_t g = (px[1]*a + dg*(255-a))/255;
            uint32_t b = (px[2]*a + db*(255-a))/255;
            *row++ = 0xFF000000u | (r<<16) | (g<<8) | b;
        }
    }
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n); if (fread(buf,1,n,f)!=(size_t)n) return 1; fclose(f);
    GifFrame *fr = NULL; int nf = 0, w = 0, h = 0;
    gif_decode_anim(buf, n, &fr, &nf, &w, &h);
    printf("nf=%d %dx%d\n", nf, w, h);
    /* paint dest white like the page bg */
    for (int i = 0; i < 800*600; i++) surf[i] = 0xFFFFFFFF;
    /* blit frame 0 at 100,100 natural size */
    blit_img(100, 100, w, h, fr[0].rgba, w, h, 100, 100, w, h);
    /* count: pixels inside image rect that stayed pure white vs changed */
    int white=0, dark=0, other=0;
    for (int y = 100; y < 100+h; y++)
        for (int x = 100; x < 100+w; x++) {
            uint32_t p = surf[y*SW+x];
            if (p == 0xFFFFFFFF) white++;
            else if ((p & 0xFFFFFF) < 0x101010) dark++;
            else other++;
        }
    printf("inside rect: white(untouched)=%d dark=%d other=%d\n", white, dark, other);
    /* sample corners */
    printf("corner px: %08x %08x %08x %08x\n",
        surf[100*SW+100], surf[100*SW+101], surf[101*SW+100], surf[150*SW+150]);
    return 0;
}
