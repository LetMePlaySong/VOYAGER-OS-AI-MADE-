/* Minimal PNG decoder (8-bit, non-interlaced, color types 0/2/3/4/6)
 * with embedded zlib/inflate implementation. Outputs RGBA8888.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "../kernel/k.h"
#include "../kernel/fb.h"

void *kmalloc(size_t size);
void kfree(void *ptr);
void *kzalloc(size_t size);

/* ===================== bit reader ===================== */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
} bitreader;

static int br_read_bit(bitreader *br) {
    if (br->bitcnt == 0) {
        if (br->pos >= br->len) return -1;
        br->bitbuf = br->data[br->pos++];
        br->bitcnt = 8;
    }
    int b = br->bitbuf & 1;
    br->bitbuf >>= 1;
    br->bitcnt--;
    return b;
}

static int br_read_bits(bitreader *br, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = br_read_bit(br);
        if (b < 0) return -1;
        v |= b << i;
    }
    return v;
}

static int br_read_byte(bitreader *br) {
    br->bitcnt = 0;
    br->bitbuf = 0;
    if (br->pos >= br->len) return -1;
    return br->data[br->pos++];
}

/* ===================== huffman ===================== */

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huffman;

static int huff_build(huffman *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        if (lens[i]) h->count[lens[i]]++;
    }
    if (h->count[0] == (uint16_t)n) return -1;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) {
        int l = lens[sym];
        if (l) h->symbol[offs[l]++] = (uint16_t)sym;
    }
    return 0;
}

static int huff_decode(bitreader *br, const huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = br_read_bit(br);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int build_fixed(huffman *lit, huffman *dist) {
    uint8_t ll[288], dl[32];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 32; i++) dl[i] = 5;
    if (huff_build(lit, ll, 288)) return -1;
    if (huff_build(dist, dl, 32)) return -1;
    return 0;
}

/* ===================== inflate ===================== */

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

int g_zfail;   /* inflate failure reason, 0 = none */

static int inflate_stored(bitreader *br, uint8_t *out, size_t outcap, size_t *outpos) {
    br->bitcnt = 0;
    br->bitbuf = 0;
    int l0 = br_read_byte(br), l1 = br_read_byte(br);
    int n0 = br_read_byte(br), n1 = br_read_byte(br);
    if (l0 < 0 || l1 < 0 || n0 < 0 || n1 < 0) { g_zfail = 120; return -1; }
    int n = l0 | (l1 << 8);
    int nn = n0 | (n1 << 8);
    if ((n ^ 0xFFFF) != nn) { g_zfail = 121; return -1; }
    if (br->pos + (size_t)n > br->len) { g_zfail = 122; return -1; }
    if (*outpos + (size_t)n > outcap) { g_zfail = 123; return -1; }
    memcpy(out + *outpos, br->data + br->pos, (size_t)n);
    br->pos += (size_t)n;
    *outpos += (size_t)n;
    return 0;
}

static int inflate_huff(bitreader *br, const huffman *lit, const huffman *dist,
                         uint8_t *out, size_t outcap, size_t *outpos) {
    for (;;) {
        int sym = huff_decode(br, lit);
        if (sym < 0) { g_zfail = 100 + br->pos; return -1; }
        if (sym < 256) {
            if (*outpos >= outcap) { g_zfail = 200 + (int)(*outpos & 0xffff); return -1; }
            out[(*outpos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else if (sym <= 285) {
            int li = sym - 257;
            int len = len_base[li];
            if (len_extra[li]) {
                int e = br_read_bits(br, len_extra[li]);
                if (e < 0) { g_zfail = 300; return -1; }
                len += e;
            }
            int dsym = huff_decode(br, dist);
            if (dsym < 0 || dsym > 29) { g_zfail = 400 + (dsym < 0 ? 0 : dsym); return -1; }
            int d = dist_base[dsym];
            if (dist_extra[dsym]) {
                int e = br_read_bits(br, dist_extra[dsym]);
                if (e < 0) { g_zfail = 500; return -1; }
                d += e;
            }
            if ((size_t)d > *outpos || *outpos + (size_t)len > outcap) { g_zfail = 600 + (int)(*outpos & 0xffff); return -1; }
            for (int i = 0; i < len; i++) {
                out[*outpos] = out[*outpos - d];
                (*outpos)++;
            }
        } else {
            g_zfail = 700 + sym;
            return -1;
        }
    }
}

static int inflate_dynamic(bitreader *br, uint8_t *out, size_t outcap, size_t *outpos) {
    int hlit = br_read_bits(br, 5) + 257;
    int hdist = br_read_bits(br, 5) + 1;
    int hclen = br_read_bits(br, 4) + 4;
    if (hlit < 0 || hdist < 0 || hclen < 0) { g_zfail = 10; return -1; }
    if (hlit > 286 || hdist > 30) { g_zfail = 11 + hlit * 64 + hdist; return -1; }
    static const uint8_t cl_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    uint8_t cl_lens[19];
    memset(cl_lens, 0, sizeof(cl_lens));
    for (int i = 0; i < hclen; i++) {
        int v = br_read_bits(br, 3);
        if (v < 0) return -1;
        cl_lens[cl_order[i]] = (uint8_t)v;
    }
    huffman cl_huff;
    if (huff_build(&cl_huff, cl_lens, 19)) { g_zfail = 20; return -1; }

    uint8_t lens[288 + 32];
    int idx = 0, total = hlit + hdist;
    while (idx < total) {
        int sym = huff_decode(br, &cl_huff);
        if (sym < 0) { g_zfail = 30 + br->pos; return -1; }
        if (sym < 16) {
            lens[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (idx == 0) { g_zfail = 40; return -1; }
            int rep = br_read_bits(br, 2);
            if (rep < 0) { g_zfail = 41; return -1; }
            rep += 3;
            uint8_t prev = lens[idx - 1];
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = prev;
        } else if (sym == 17) {
            int rep = br_read_bits(br, 3);
            if (rep < 0) { g_zfail = 42; return -1; }
            rep += 3;
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = 0;
        } else if (sym == 18) {
            int rep = br_read_bits(br, 7);
            if (rep < 0) { g_zfail = 43; return -1; }
            rep += 11;
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = 0;
        } else {
            g_zfail = 50 + sym;
            return -1;
        }
    }
    if (idx != total) { g_zfail = 60 + idx; return -1; }

    huffman lit_huff, dist_huff;
    if (huff_build(&lit_huff, lens, hlit)) { g_zfail = 70; return -1; }
    /* distance code with all-zero lengths is legal if no distances used;
       huff_build rejects all-zero; handle: if hdist==1 && lens[hlit]==0,
       build a dummy */
    if (hdist == 1 && lens[hlit] == 0) {
        memset(dist_huff.count, 0, sizeof(dist_huff.count));
        dist_huff.count[1] = 1;
        dist_huff.symbol[0] = 0;
        /* mark as incomplete-but-usable: our decoder tolerates */
    } else if (huff_build(&dist_huff, lens + hlit, hdist)) {
        /* try padding: some encoders emit hdist=1 len=0 */
        g_zfail = 80;
        return -1;
    }
    return inflate_huff(br, &lit_huff, &dist_huff, out, outcap, outpos);
}

static long zlib_inflate(const uint8_t *src, size_t srclen,
                         uint8_t *dst, size_t dstcap) {
    g_zfail = 0;
    if (srclen < 2) { g_zfail = 1; return -1; }
    uint32_t cmf_flg = ((uint32_t)src[0] << 8) | src[1];
    if ((cmf_flg % 31) != 0 || (src[0] & 0x0F) != 8) { g_zfail = 2; return -1; }
    if (src[1] & 0x20) { g_zfail = 3; return -1; } /* FLG.FDICT */

    bitreader br;
    br.data = src + 2;
    br.len = srclen - 2;
    br.pos = 0;
    br.bitbuf = 0;
    br.bitcnt = 0;

    size_t outpos = 0;
    for (;;) {
        int bfinal = br_read_bit(&br);
        int btype = br_read_bits(&br, 2);
        if (bfinal < 0 || btype < 0) return -1;
        int rc;
        if (btype == 0) rc = inflate_stored(&br, dst, dstcap, &outpos);
        else if (btype == 1) {
            huffman lit, dist;
            if (build_fixed(&lit, &dist)) return -1;
            rc = inflate_huff(&br, &lit, &dist, dst, dstcap, &outpos);
        } else if (btype == 2) {
            rc = inflate_dynamic(&br, dst, dstcap, &outpos);
        } else { g_zfail = 33; return -1; }   /* reserved btype=3 */
        if (rc) return -1;
        if (bfinal) break;
    }
    br.bitcnt = 0;
    br.bitbuf = 0;
    if (br.pos + 4 <= br.len) {
        uint32_t adler = ((uint32_t)br.data[br.pos] << 24) |
                         ((uint32_t)br.data[br.pos+1] << 16) |
                         ((uint32_t)br.data[br.pos+2] << 8) |
                         br.data[br.pos+3];
        uint32_t s1 = 1, s2 = 0;
        for (size_t i = 0; i < outpos; i++) {
            s1 = (s1 + dst[i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        uint32_t calc = (s2 << 16) | s1;
        if (calc != adler)
            kprintf("PNG adler mismatch (non-fatal) calc=%x want=%x", calc, adler);
    }
    return (long)outpos;
}

/* ===================== PNG ===================== */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t paeth(uint32_t a, uint32_t b, uint32_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* extract one sample (0..255) from an unfiltered pass row.
 * Handles 8/16-bit samples and sub-byte packing (bitdepth 1/2/4). */
/* raw palette index (NOT scaled to 0-255 like png_sample) - palette
 * indices are indexes, not intensities */
static int png_index(const uint8_t *row, int x, int bitdepth) {
    if (bitdepth == 8) return row[x];
    if (bitdepth == 16) {
        int v = ((int)row[2 * x] << 8) | row[2 * x + 1];
        return v >> 8;
    }
    int bit = x * bitdepth, v = 0;
    for (int k = 0; k < bitdepth; k++) {
        int b = bit + k;
        v = (v << 1) | ((row[b >> 3] >> (7 - (b & 7))) & 1);
    }
    return v;
}

static int png_sample(const uint8_t *row, int x, int ch,
                      int channels, int bitdepth) {
    if (bitdepth == 16) {
        int bit = (x * channels + ch) * 16;
        int v = ((int)row[bit >> 3] << 8) | row[(bit >> 3) + 1];
        return v >> 8;
    }
    if (bitdepth == 8) return row[x * channels + ch];
    int bit = (x * channels + ch) * bitdepth;
    int v = 0;
    for (int k = 0; k < bitdepth; k++) {
        int b = bit + k;
        v = (v << 1) | ((row[b >> 3] >> (7 - (b & 7))) & 1);
    }
    int max = (1 << bitdepth) - 1;
    return (v * 255 + max / 2) / max;
}

static int png_decode_int(const uint8_t *data, size_t size,
               uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    if (size < 8 || memcmp(data, sig, 8)) return -1;

    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    uint8_t plte[768];
    int plte_n = 0;
    uint8_t trns[256];
    int trns_n = 0;

    while (pos + 12 <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *chunk = data + pos + 8;
        if (pos + 12 + len > size) break;
        if (!memcmp(type, "IHDR", 4)) {
            w = be32(chunk);
            h = be32(chunk + 4);
            bitdepth = chunk[8];
            colortype = chunk[9];
            interlace = chunk[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            plte_n = (int)(len / 3);
            if (plte_n > 256) plte_n = 256;
            memcpy(plte, chunk, (size_t)plte_n * 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            trns_n = (int)len;
            if (trns_n > 256) trns_n = 256;
            memcpy(trns, chunk, (size_t)trns_n);
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + len > idat_cap) {
                idat_cap = (idat_len + len) * 2 + 4096;
                uint8_t *ni = kmalloc(idat_cap);
                if (!ni) { kfree(idat); return -1; }
                if (idat) { memcpy(ni, idat, idat_len); kfree(idat); }
                idat = ni;
            }
            memcpy(idat + idat_len, chunk, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }

    int channels;
    switch (colortype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: kfree(idat); return -1;
    }
    /* bit depth legality: gray/palette allow 1/2/4/8/16, others 8/16.
     * Sub-8 depths pack samples — handled by png_sample below. */
    bool bdok = (colortype == 0 || colortype == 3)
        ? (bitdepth >= 1 && bitdepth <= 16 && bitdepth != 3 &&
           bitdepth != 5 && bitdepth != 6 && bitdepth != 7)
        : (bitdepth == 8 || bitdepth == 16);
    if (!bdok || !idat || !w || !h || w > 16384 || h > 16384) {
        kfree(idat); return -1;
    }

    /* Adam7 pass table: x0,y0,dx,dy — index 7 = identity (non-interlaced) */
    static const int X0[8] = {0,4,0,2,0,1,0,0};
    static const int Y0[8] = {0,0,4,0,2,0,1,0};
    static const int DX[8] = {8,8,4,4,2,2,1,1};
    static const int DY[8] = {8,8,8,4,4,2,2,1};
    int npass = interlace ? 7 : 1;
    int bpp = bitdepth == 16 ? channels * 2
            : bitdepth == 8 ? channels : 1;   /* filter bytes-per-pixel */

    /* total inflated size: per pass, ph rows of (filter + packed samples) */
    size_t rawcap = 0;
    int pw[8], ph[8], rb[8];
    for (int p = 0; p < npass; p++) {
        int pi = interlace ? p : 7;
        int x0 = X0[pi], y0 = Y0[pi], dx = DX[pi], dy = DY[pi];
        pw[p] = w > (uint32_t)x0 ? (int)((w - x0 + dx - 1) / dx) : 0;
        ph[p] = h > (uint32_t)y0 ? (int)((h - y0 + dy - 1) / dy) : 0;
        rb[p] = pw[p] > 0 ? (pw[p] * channels * bitdepth + 7) / 8 : 0;
        rawcap += (size_t)ph[p] * (rb[p] + 1);
    }
    if (rawcap == 0 || rawcap > (size_t)w * h * channels * 2 + 4096) {
        kfree(idat); return -1;
    }
    uint8_t *raw = kmalloc(rawcap);
    if (!raw) { kfree(idat); return -1; }

    long dlen = zlib_inflate(idat, idat_len, raw, rawcap);
    kfree(idat);
    if (dlen < (long)rawcap) { kfree(raw); return -1; }

    uint8_t *rgba = kmalloc((size_t)w * h * 4);
    if (!rgba) { kfree(raw); return -1; }
    memset(rgba, 0, (size_t)w * h * 4);

    const uint8_t *stream = raw;
    uint8_t *prev = NULL, *cur = NULL;
    static uint8_t *bigprev = NULL, *bigcur = NULL;
    static int bigcap = 0;

    for (int p = 0; p < npass; p++) {
        if (pw[p] <= 0 || ph[p] <= 0) continue;
        int pi = interlace ? p : 7;
        int x0 = X0[pi], y0 = Y0[pi], dx = DX[pi], dy = DY[pi];
        int stride = rb[p];
        /* (re)size unfilter buffers if this pass is wider */
        if (stride > bigcap) {
            kfree(bigprev); kfree(bigcur);
            bigcap = stride * 2 + 16;
            bigprev = kzalloc((size_t)bigcap);
            bigcur = kzalloc((size_t)bigcap);
            if (!bigprev || !bigcur) { kfree(raw); kfree(rgba); kfree(bigprev); kfree(bigcur); bigprev=bigcur=NULL; bigcap=0; return -1; }
        }
        prev = bigprev; cur = bigcur;
        memset(prev, 0, (size_t)stride);
        memset(cur, 0, (size_t)stride);

        for (int y = 0; y < ph[p]; y++) {
            const uint8_t *src = stream;
            stream += stride + 1;
            int filter = src[0];
            const uint8_t *fd = src + 1;
            uint8_t *dst = cur;
            for (int i = 0; i < stride; i++) {
                int a = i >= bpp ? dst[i - bpp] : 0;
                int b = prev[i];
                int c = i >= bpp ? prev[i - bpp] : 0;
                int x = fd[i];
                int val;
                switch (filter) {
                    case 0: val = x; break;
                    case 1: val = x + a; break;
                    case 2: val = x + b; break;
                    case 3: val = x + ((a + b) >> 1); break;
                    case 4: val = x + (int)paeth((uint32_t)a, (uint32_t)b, (uint32_t)c); break;
                    default: val = x; break;
                }
                dst[i] = (uint8_t)(val & 0xFF);
            }
            /* scatter this pass row into the image */
            int iy = y0 + y * dy;
            for (int x = 0; x < pw[p]; x++) {
                int ix = x0 + x * dx;
                if ((uint32_t)ix >= w || (uint32_t)iy >= h) continue;
                uint8_t *o = rgba + ((size_t)iy * w + ix) * 4;
                int sv[4] = {0,0,0,255};
                for (int ch = 0; ch < channels; ch++)
                    sv[ch] = png_sample(cur, x, ch, channels, bitdepth);
                o[3] = 255;   /* default opaque; tRNS/alpha channels override */
                switch (colortype) {
                case 0: {
                    o[0] = o[1] = o[2] = (uint8_t)sv[0];
                    /* tRNS gray at ANY bit depth: the chunk stores the
                     * sample at image bit depth - scale it to 0-255 the
                     * same way png_sample does, or 1/2/4-bit grayscale
                     * PNGs with transparency render as opaque boxes */
                    if (trns_n >= 2) {
                        int tv = (trns[0] << 8) | trns[1];
                        int cmp;
                        if (bitdepth == 16) cmp = tv >> 8;
                        else if (bitdepth == 8) cmp = tv & 0xFF;
                        else {
                            int mx = (1 << bitdepth) - 1;
                            cmp = (tv * 255 + mx / 2) / mx;
                        }
                        if (sv[0] == cmp) o[3] = 0;
                    }
                    break;
                }
                case 2: {
                    o[0] = sv[0]; o[1] = sv[1]; o[2] = sv[2];
                    if (trns_n >= 6 && bitdepth >= 8) {
                        int tr = ((trns[0]<<8)|trns[1]) >> (bitdepth==16?8:0);
                        int tg = ((trns[2]<<8)|trns[3]) >> (bitdepth==16?8:0);
                        int tb = ((trns[4]<<8)|trns[5]) >> (bitdepth==16?8:0);
                        if (sv[0]==(tr&0xFF) && sv[1]==(tg&0xFF) && sv[2]==(tb&0xFF))
                            o[3] = 0;
                    }
                    break;
                }
                case 3: {
                    /* raw index - png_sample would scale 4-bit 0-15 up
                     * to 0-255 and pick completely wrong palette/tRNS
                     * entries (the "transparent PNG shows wrong colors"
                     * bug on sub-8-bit palette files) */
                    int idx = png_index(cur, x, bitdepth);
                    if (idx < 0) idx = 0;
                    /* malformed/short PLTE: an out-of-range index used
                     * to leave RGB=0,0,0 with alpha 255 = a black box */
                    if (plte_n > 0 && idx >= plte_n) idx = plte_n - 1;
                    if (idx * 3 + 2 < plte_n * 3) {
                        o[0] = plte[idx*3]; o[1] = plte[idx*3+1]; o[2] = plte[idx*3+2];
                    }
                    if (idx < trns_n) o[3] = trns[idx];
                    break;
                }
                case 4: o[0]=o[1]=o[2]=(uint8_t)sv[0]; o[3]=(uint8_t)sv[1]; break;
                case 6: o[0]=sv[0]; o[1]=sv[1]; o[2]=sv[2]; o[3]=sv[3]; break;
                }
            }
            uint8_t *tmp = prev; prev = cur; cur = tmp;
            memset(cur, 0, (size_t)stride);
        }
        stream += 0;
    }
    kfree(raw);
    kfree(bigprev); kfree(bigcur); bigprev = bigcur = NULL; bigcap = 0;
    (void)prev; (void)cur;

    *out_rgba = rgba;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* Callers mask IRQs around the decode (see image_data in gui.cpp): the
 * decoder is CPU-bound and a timer/keyboard interrupt mid-run corrupts
 * its state. png_decode itself does not touch IF. */
int png_decode(const uint8_t *data, size_t size,
               uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    return png_decode_int(data, size, out_rgba, out_w, out_h);
}
