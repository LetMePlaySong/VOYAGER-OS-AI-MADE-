/* Minimal GIF decoder -> RGBA8888. First frame only; handles global/local
 * color tables, LZW, transparency and interlacing. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

typedef struct {
    const uint8_t *d; uint32_t len, pos;
} G;
static uint16_t rd16(G *g) {
    if (g->pos + 2 > g->len) { g->pos = g->len; return 0; }
    uint16_t v = g->d[g->pos] | (g->d[g->pos+1] << 8); g->pos += 2; return v;
}
static uint8_t rd8(G *g) { return g->pos < g->len ? g->d[g->pos++] : 0; }

/* LZW dictionary: prefix/suffix/next tables sized for 12-bit codes */
#define GDIC 4096
typedef struct {
    uint16_t prefix[GDIC];
    uint8_t  suffix[GDIC];
    uint16_t next[GDIC];
    uint8_t  used[GDIC];
} Dict;

static int gif_lzw(G *g, uint8_t *out, int outcap, int expect,
                   int min_code, Dict *dc) {
    /* bit reader LSB-first */
    uint32_t bitbuf = 0; int bitcnt = 0;
    int clear = 1 << min_code, eoi = clear + 1;
    int code_size = min_code + 1;
    int free_ent = eoi + 1;
    memset(dc->used, 0, sizeof(dc->used));
    int prev = -1, produced = 0;

    for (;;) {
        while (bitcnt < code_size) {
            if (g->pos >= g->len) return -1;
            bitbuf |= (uint32_t)g->d[g->pos++] << bitcnt;
            bitcnt += 8;
        }
        int code = bitbuf & ((1 << code_size) - 1);
        bitbuf >>= code_size; bitcnt -= code_size;
        if (code == clear) {
            code_size = min_code + 1;
            free_ent = eoi + 1;
            memset(dc->used, 0, sizeof(dc->used));  /* dict resets on CLEAR */
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        /* reject codes beyond the dictionary (corrupt stream); code==
         * free_ent is the legal KwKwK case handled below */
        if (code > free_ent || code >= GDIC) return -1;
        /* code==free_ent is the KwKwK case (handled below); anything else
         * above eoi must already be a used dictionary entry */
        if (code > eoi && code != free_ent && !dc->used[code]) return -1;

        /* decode the string for `code` into stack[] REVERSED (stack top =
         * first output char). Literals are codes <= eoi; dict entries are
         * > eoi and walk prefix/suffix chains. */
        int stack[4096], sp = 0;
        int kwkwk = (code == free_ent && prev >= 0);
        int c = kwkwk ? prev : code;
        while (c > eoi) {
            if (!dc->used[c] || sp >= 4000) return -1;   /* cycle/corrupt */
            stack[sp++] = dc->suffix[c];
            c = dc->prefix[c];
            if (c < 0) return -1;
        }
        if (c < 0 || c >= clear) return -1;    /* terminator must be literal */
        stack[sp++] = (uint8_t)c;
        if (kwkwk) {
            /* String(code) = String(prev) + firstchar(String(prev)).
             * firstchar is stack[sp-1]; the extra char belongs at the END
             * of the forward string = bottom of the reversed stack, so
             * shift everything up one and drop it at index 0. */
            if (sp >= 4000) return -1;
            memmove(stack + 1, stack, (size_t)sp * sizeof(stack[0]));
            stack[0] = stack[sp];   /* old top = firstchar(String(prev)) */
            sp++;
        }
        /* output reversed stack = forward string */
        for (int i = sp - 1; i >= 0; i--) {
            if (produced >= outcap) return -1;
            out[produced++] = (uint8_t)stack[i];
        }
        if (prev >= 0 && free_ent < GDIC) {
            dc->prefix[free_ent] = (uint16_t)prev;
            /* first char of current string = stack[sp-1] (top) */
            dc->suffix[free_ent] = (uint8_t)stack[sp - 1];
            dc->used[free_ent] = 1;
            free_ent++;
            /* bump code width when the table reaches what code_size bits
             * can index (verified against real GIFs: '>' desyncs the
             * bitstream, '>=' is correct) */
            if (free_ent >= (1 << code_size) && code_size < 12)
                code_size++;
        }
        prev = code;
        if (produced >= expect) break;
    }
    return produced;
}

/* one decoded animation frame: full logical-screen canvas snapshot */
typedef struct {
    uint8_t *rgba;      /* w*h*4, owned by caller */
    int delay_cs;       /* GCE delay in hundredths of a second */
} GifFrame;

/* decode EVERY frame of an animated GIF. Each frame is a full-canvas
 * RGBA snapshot (transparent-index pixels leave the canvas untouched,
 * so frames composite like a real browser). Bounded: stops adding
 * frames past the byte/count budget and returns what it has. */
int gif_decode_anim(const uint8_t *d, uint32_t n,
                    GifFrame **out_frames, int *out_n,
                    int *ow, int *oh) {
    if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') return -1;
    G g = { d, n, 6 };   /* skip header */
    uint16_t w = rd16(&g), h = rd16(&g);
    uint8_t packed = rd8(&g);
    g.pos += 2;          /* bg + aspect */
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    int gct = packed & 0x80, gct_n = 2 << (packed & 7);
    uint8_t pal[768];
    if (gct) {
        if (g.pos + gct_n * 3 > n) return -1;
        memcpy(pal, d + g.pos, gct_n * 3); g.pos += gct_n * 3;
    } else memset(pal, 0, sizeof(pal));
    int bg_idx = g.d[11] & 0xFF;         /* LSD background color index */
    if (bg_idx >= gct_n) bg_idx = 0;

    /* persistent canvas: background colour with alpha 0 - spec fills it
     * opaque, but for page rendering transparent margins/holes should
     * show the PAGE through, and frame-1 transparent pixels must too */
    uint8_t *canvas = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!canvas) return -1;
    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        canvas[i*4+0] = pal[bg_idx*3+0];
        canvas[i*4+1] = pal[bg_idx*3+1];
        canvas[i*4+2] = pal[bg_idx*3+2];
        canvas[i*4+3] = 0;
    }

    GifFrame *frames = NULL;
    int nfr = 0, capfr = 0;
    size_t frame_bytes = 0;
    const size_t FRAME_BUDGET = 8u << 20;   /* 8MB of RGBA per GIF max */
    int transp_on = 0, transp_idx = 0, delay_cs = 10, dispose = 0;
    Dict *dc = (Dict *)kmalloc(sizeof(Dict));
    if (!dc) { kfree(canvas); return -1; }

    for (;;) {
        if (g.pos + 1 > n) break;
        uint8_t b = rd8(&g);
        if (b == 0x3B) break;                    /* trailer */
        if (b == 0x21) {                          /* extension */
            uint8_t label = rd8(&g);
            if (label == 0xF9) {
                uint8_t sz = rd8(&g);
                if (sz >= 4 && g.pos + sz <= n) {
                    uint8_t pk = g.d[g.pos];
                    transp_on = pk & 1;
                    dispose = (pk >> 2) & 7;
                    delay_cs = g.d[g.pos+1] | (g.d[g.pos+2] << 8);
                    transp_idx = g.d[g.pos+3];
                }
                if (g.pos + sz <= n) g.pos += sz;
            }
            for (;;) {
                uint8_t sz = rd8(&g);
                if (!sz) break;
                if (g.pos + sz > n) goto fail;
                g.pos += sz;
            }
            continue;
        }
        if (b != 0x2C) { g.pos++; continue; }
        /* image descriptor */
        uint16_t ix = rd16(&g), iy = rd16(&g);
        uint16_t iw = rd16(&g), ih = rd16(&g);
        uint8_t ipk = rd8(&g);
        if (iw > 2048 || ih > 2048 || (uint64_t)iw * ih > (1u << 22))
            goto fail;
        int lct = ipk & 0x80, inter = ipk & 0x40;
        int lct_n = 2 << (ipk & 7);
        uint8_t lp[768];
        const uint8_t *use_pal = pal;
        int pn = gct_n;
        if (lct) {
            if (g.pos + lct_n * 3 > n) goto fail;
            memcpy(lp, d + g.pos, lct_n * 3); g.pos += lct_n * 3;
            use_pal = lp; pn = lct_n;
        }
        int min_code = rd8(&g);
        if (min_code < 2 || min_code > 8) goto fail;
        uint32_t cap = iw * ih + 64;
        uint8_t *raw = (uint8_t *)kmalloc(cap);
        if (!raw) goto fail;
        int got = 0;
        for (;;) {
            uint8_t sz = rd8(&g);
            if (!sz) break;
            if (got + sz > (int)cap || g.pos + sz > n) { kfree(raw); goto fail; }
            memcpy(raw + got, d + g.pos, sz); got += sz; g.pos += sz;
        }
        uint8_t *idx = (uint8_t *)kmalloc((size_t)iw * ih + 16);
        if (!idx) { kfree(raw); goto fail; }
        G sub = { raw, (uint32_t)got, 0 };
        int produced = gif_lzw(&sub, idx, iw * ih, iw * ih, min_code, dc);
        kfree(raw);
        if (produced < iw * ih) { kfree(idx); goto fail; }

        /* composite this frame's rect onto the persistent canvas.
         * transparent-index pixels are SKIPPED (leave previous canvas)
         * - that is what makes frame N build on frame N-1. */
        static const int pstart[4] = { 0, 4, 2, 1 };
        static const int pstep[4]  = { 8, 8, 4, 2 };
        int npass = inter ? 4 : 1;
        int src = 0;
        for (int p = 0; p < npass && src < iw * ih; p++) {
            int rows = inter ? ((ih > pstart[p]) ?
                        (ih - pstart[p] + pstep[p] - 1) / pstep[p] : 0)
                             : ih;
            for (int r = 0; r < rows && src < iw * ih; r++) {
                int dy = inter ? pstart[p] + r * pstep[p] : r;
                for (int px = 0; px < iw; px++) {
                    int ci = idx[src++];
                    int fx = ix + px, fy = iy + dy;
                    if (fx >= (int)w || fy >= (int)h || fx < 0 || fy < 0)
                        continue;
                    if (transp_on && ci == transp_idx) continue;
                    if (ci >= pn) ci = 0;
                    uint8_t *o = canvas + ((size_t)fy * w + fx) * 4;
                    o[0] = use_pal[ci*3+0]; o[1] = use_pal[ci*3+1];
                    o[2] = use_pal[ci*3+2]; o[3] = 255;
                }
            }
        }
        kfree(idx);

        /* snapshot the canvas as this frame's image */
        size_t need = (size_t)w * h * 4;
        if (nfr >= 64 || frame_bytes + need > FRAME_BUDGET) break;
        if (nfr >= capfr) {
            int nc = capfr ? capfr * 2 : 8;
            GifFrame *nf = (GifFrame *)krealloc(frames,
                            (size_t)nc * sizeof(GifFrame));
            if (!nf) break;
            frames = nf; capfr = nc;
        }
        uint8_t *snap = (uint8_t *)kmalloc(need);
        if (!snap) break;
        memcpy(snap, canvas, need);
        frames[nfr].rgba = snap;
        frames[nfr].delay_cs = delay_cs > 0 ? delay_cs : 10;
        nfr++;
        frame_bytes += need;

        /* disposal method 2: restore the frame rect to the background
         * colour after this frame has been shown. Alpha stays 0 like
         * the initial canvas - an opaque restore would punch a solid
         * (usually black) rect into later frames where the page should
         * show through instead */
        if (dispose == 2) {
            for (int yy = iy; yy < iy + ih && yy < (int)h; yy++) {
                if (yy < 0) continue;
                for (int xx = ix; xx < ix + iw && xx < (int)w; xx++) {
                    if (xx < 0) continue;
                    uint8_t *o = canvas + ((size_t)yy * w + xx) * 4;
                    o[0] = pal[bg_idx*3+0]; o[1] = pal[bg_idx*3+1];
                    o[2] = pal[bg_idx*3+2]; o[3] = 0;
                }
            }
        }
        /* dispose 3 (restore previous) unsupported - canvas keeps state,
         * close enough for the vast majority of real-world GIFs */

        /* GCE scope ends at the first image block: the NEXT frame must
         * not inherit this frame's transparent index/delay/disposal or
         * its pixels get wrongly knocked out */
        transp_on = 0; transp_idx = 0; delay_cs = 10; dispose = 0;
    }
    kfree(dc);
    kfree(canvas);
    if (nfr <= 0) { if (frames) kfree(frames); return -1; }
    *out_frames = frames; *out_n = nfr; *ow = w; *oh = h;
    return 0;

fail:
    kfree(dc); kfree(canvas);
    for (int i = 0; i < nfr; i++) kfree(frames[i].rgba);
    if (frames) kfree(frames);
    return -1;
}

int gif_decode(const uint8_t *d, uint32_t n,
               uint8_t **out, int *ow, int *oh) {
    GifFrame *frames = NULL; int nfr = 0, w = 0, h = 0;
    if (gif_decode_anim(d, n, &frames, &nfr, &w, &h) != 0 || nfr <= 0)
        return -1;
    /* hand frame 0 to the caller; free the rest */
    *out = frames[0].rgba; *ow = w; *oh = h;
    for (int i = 1; i < nfr; i++) kfree(frames[i].rgba);
    kfree(frames);
    return 0;
}
