/* Minimal PNG decoder (8-bit, non-interlaced, color types 0/2/3/4/6)
 * with embedded zlib/inflate implementation. Outputs RGBA8888.
 */

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

static int inflate_stored(bitreader *br, uint8_t *out, size_t outcap, size_t *outpos) {
    br->bitcnt = 0;
    br->bitbuf = 0;
    int l0 = br_read_byte(br), l1 = br_read_byte(br);
    int n0 = br_read_byte(br), n1 = br_read_byte(br);
    if (l0 < 0 || l1 < 0 || n0 < 0 || n1 < 0) return -1;
    int n = l0 | (l1 << 8);
    int nn = n0 | (n1 << 8);
    if ((n ^ 0xFFFF) != nn) return -1;
    if (br->pos + (size_t)n > br->len) return -1;
    if (*outpos + (size_t)n > outcap) return -1;
    memcpy(out + *outpos, br->data + br->pos, (size_t)n);
    br->pos += (size_t)n;
    *outpos += (size_t)n;
    return 0;
}

static int inflate_huff(bitreader *br, const huffman *lit, const huffman *dist,
                         uint8_t *out, size_t outcap, size_t *outpos) {
    for (;;) {
        int sym = huff_decode(br, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*outpos >= outcap) return -1;
            out[(*outpos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else if (sym <= 285) {
            int li = sym - 257;
            int len = len_base[li];
            if (len_extra[li]) {
                int e = br_read_bits(br, len_extra[li]);
                if (e < 0) return -1;
                len += e;
            }
            int dsym = huff_decode(br, dist);
            if (dsym < 0 || dsym > 29) return -1;
            int d = dist_base[dsym];
            if (dist_extra[dsym]) {
                int e = br_read_bits(br, dist_extra[dsym]);
                if (e < 0) return -1;
                d += e;
            }
            if ((size_t)d > *outpos || *outpos + (size_t)len > outcap) return -1;
            for (int i = 0; i < len; i++) {
                out[*outpos] = out[*outpos - d];
                (*outpos)++;
            }
        } else {
            return -1;
        }
    }
}

static int inflate_dynamic(bitreader *br, uint8_t *out, size_t outcap, size_t *outpos) {
    int hlit = br_read_bits(br, 5) + 257;
    int hdist = br_read_bits(br, 5) + 1;
    int hclen = br_read_bits(br, 4) + 4;
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    if (hlit > 286 || hdist > 30) return -1;
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
    if (huff_build(&cl_huff, cl_lens, 19)) return -1;

    uint8_t lens[288 + 32];
    int idx = 0, total = hlit + hdist;
    while (idx < total) {
        int sym = huff_decode(br, &cl_huff);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (idx == 0) return -1;
            int rep = br_read_bits(br, 2);
            if (rep < 0) return -1;
            rep += 3;
            uint8_t prev = lens[idx - 1];
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = prev;
        } else if (sym == 17) {
            int rep = br_read_bits(br, 3);
            if (rep < 0) return -1;
            rep += 3;
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = 0;
        } else if (sym == 18) {
            int rep = br_read_bits(br, 7);
            if (rep < 0) return -1;
            rep += 11;
            for (int i = 0; i < rep && idx < total; i++) lens[idx++] = 0;
        } else {
            return -1;
        }
    }
    if (idx != total) return -1;

    huffman lit_huff, dist_huff;
    if (huff_build(&lit_huff, lens, hlit)) return -1;
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
        return -1;
    }
    return inflate_huff(br, &lit_huff, &dist_huff, out, outcap, outpos);
}

static long zlib_inflate(const uint8_t *src, size_t srclen,
                         uint8_t *dst, size_t dstcap) {
    if (srclen < 2) return -1;
    uint32_t cmf_flg = ((uint32_t)src[0] << 8) | src[1];
    if ((cmf_flg % 31) != 0 || (src[0] & 0x0F) != 8) return -1;
    if (src[1] & 0x20) return -1; /* FLG.FDICT - preset dictionary unsupported */

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
        } else return -1;
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

int png_decode(const uint8_t *data, size_t size,
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

    if (!w || !h || bitdepth != 8 || interlace != 0 || !idat) {
        kfree(idat);
        return -1;
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
    int stride = w * channels;
    if (stride <= 0 || w > 8192 || h > 8192) { kfree(idat); return -1; }
    size_t rawcap = ((size_t)stride + 1) * h;
    kprintf("png: stride=%d rawcap=%u idat=%u idatp=%p", stride, (unsigned)rawcap, (unsigned)idat_len, idat);
    uint8_t *raw = kmalloc(rawcap);
    kprintf("png: raw=%p", raw);
    if (!raw) { kfree(idat); return -1; }

    long dlen = zlib_inflate(idat, idat_len, raw, rawcap);
    kprintf("png: dlen=%d", (int)dlen);
    kfree(idat);
    if (dlen < 0) { kfree(raw); return -1; }

    uint8_t *clean = kmalloc((size_t)stride * h);
    uint8_t *prev = kzalloc((size_t)stride);
    uint8_t *cur = kzalloc((size_t)stride);
    kprintf("png: clean=%p prev=%p cur=%p", clean, prev, cur);
    if (!clean || !prev || !cur) {
        kfree(raw); kfree(clean); kfree(prev); kfree(cur);
        return -1;
    }

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = raw + (size_t)y * (stride + 1);
        int filter = src[0];
        const uint8_t *fdata = src + 1;
        uint8_t *dst = clean + (size_t)y * stride;
        for (int i = 0; i < stride; i++) {
            int a = (i >= channels) ? cur[i - channels] : 0;
            int b = prev[i];
            int c = (i >= channels) ? prev[i - channels] : 0;
            int x = fdata[i];
            int val;
            switch (filter) {
                case 0: val = x; break;
                case 1: val = x + a; break;
                case 2: val = x + b; break;
                case 3: val = x + ((a + b) >> 1); break;
                case 4: val = x + (int)paeth(a, b, c); break;
                default: val = x; break;
            }
            cur[i] = (uint8_t)(val & 0xFF);
        }
        memcpy(dst, cur, (size_t)stride);
        uint8_t *tmp = prev; prev = cur; cur = tmp;
        memset(cur, 0, (size_t)stride);
    }
    kfree(raw);
    kfree(prev);
    kfree(cur);
    kprint("png: unfiltered\n");

    uint8_t *rgba = kmalloc((size_t)w * h * 4);
    kprintf("png: rgba=%p", rgba);
    if (!rgba) { kfree(clean); return -1; }
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        const uint8_t *p = clean + (size_t)i * channels;
        switch (colortype) {
            case 0: r = g = b = p[0]; break;
            case 2: r = p[0]; g = p[1]; b = p[2]; break;
            case 3: {
                int idx = p[0];
                if (idx * 3 + 2 < plte_n * 3) {
                    r = plte[idx*3]; g = plte[idx*3+1]; b = plte[idx*3+2];
                }
                if (idx < trns_n) a = trns[idx];
                break;
            }
            case 4: r = g = b = p[0]; a = p[1]; break;
            case 6: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
        }
        rgba[i*4+0] = r; rgba[i*4+1] = g; rgba[i*4+2] = b; rgba[i*4+3] = a;
    }
    kfree(clean);

    *out_rgba = rgba;
    *out_w = w;
    *out_h = h;
    return 0;
}
