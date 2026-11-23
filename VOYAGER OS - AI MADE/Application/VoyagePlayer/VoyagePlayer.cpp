/* Voyage Player-style music player - classic 2.x look (black skin, green LCD,
 * transport buttons, EQ, visualizer) playing real audio files.
 * Sources: System HD (sysvol .wav/.mp3 modules) + VFS user files.
 * Decode: WAV PCM directly, MP3 via minimp3. Output: AC97 DAC. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "../../gui/gui.h"
#include "../../kernel/k.h"
#include "../../kernel/arch/arch.h"
#include "../../kernel/sysvol.h"
#include "../../kernel/vfs.h"
#include "../../drivers/ac97.h"
#include "../../lib/minimp3.h"
#include <stdio.h>

/* ---------------- track model ---------------- */
#define MAX_TRACKS 24
#define MAX_NAME   40

enum { SRC_NONE, SRC_SYSVOL, SRC_VFS };

typedef struct {
    char     name[MAX_NAME];
    int      src;              /* SRC_SYSVOL / SRC_VFS */
    char     path[96];         /* sysvol path or vfs filename */
    char     vfs_dir[VFS_NAME_LEN + 1]; /* "" = root, else folder name */
    /* decoded stream state */
    int      kind;             /* 0 unknown, 1 wav, 2 mp3 */
    const uint8_t *data;       /* compressed/file bytes (sysvol: module) */
    uint8_t *vfs_buf;          /* kmalloc'd copy for VFS files */
    uint32_t data_len;
    /* playback position */
    uint32_t pos;              /* byte offset into data (mp3) or sample offset (wav) */
    /* wav parse results */
    uint32_t wav_rate, wav_ch, wav_bits, wav_data_off, wav_data_len;
    /* mp3 state */
    mp3dec_t mp3dec;
    int      mp3_rate, mp3_ch;
    /* output format (what we feed AC97) */
    uint32_t out_rate;
    uint64_t total_ms, elapsed_ms;
} Track;

typedef struct {
    Track    tracks[MAX_TRACKS];
    int      ntracks;
    int      track;            /* current */
    int      sel;
    int      playing, paused;
    int      have_pcm;         /* decoded chunk pending */
    int16_t *pcm;              /* decoded chunk buffer (native rate) */
    int16_t *pcm2;             /* resampled to DAC rate */
    uint32_t pcm_frames;       /* valid stereo frames in pcm */
    uint32_t pcm_pos;          /* consumed frames */
    uint32_t rframes, rpos;    /* resampled frames valid / consumed */
    uint32_t in_rate;          /* sample rate of pcm content */
    uint32_t rate;             /* DAC rate we output at (ac97_rate) */
    uint64_t pcm_ms_done;      /* ms of audio already pushed this chunk */
    uint64_t tick0;            /* gui_ticks when play started (for pos) */
    uint64_t paused_at;
    int      drag;             /* seek drag */
    int      pl_vis, pl_scroll, pl_top;  /* playlist geometry this frame */
    int      scroll;             /* persistent playlist row offset (wheel) */
    int      sbar;               /* scrollbar grab offset, -1 = not dragging */
    int      pl_bot;
    int      sb_tx, sb_ty, sb_th;       /* scrollbar track rect */
    int      sb_my, sb_mh, sb_maxsc;    /* thumb rect + max scroll */
    int      viz[AC97_SPEC_BANDS];      /* smoothed spectrum levels 0..255 */
    int      hov_btn;          /* transport button under pointer, -1 none */
    int      hov_row;          /* playlist row under pointer, -1 none */
    int      hov_sb;           /* scrollbar under pointer */
    int      prs_btn, prs_row; /* pressed transport button / playlist row */
    int      mx, my;           /* last pointer pos (content coords) */
} WmpCtx;

/* ---------------- helpers ---------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int is_audio_name(const char *n) {
    size_t l = strlen(n);
    if (l < 4) return 0;
    char ext[8];
    size_t el = l < 7 ? l : 7;
    strncpy(ext, n + l - el, el); ext[el] = 0;
    char *dot = strrchr(ext, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".wav") || !strcasecmp(dot, ".mp3") ||
           !strcasecmp(dot, ".ogg");
}

static const uint8_t *track_data(Track *t, uint32_t *len) {
    if (t->src == SRC_SYSVOL) {
        const SysVolEnt *e = sysvol_lookup(t->path);
        const uint8_t *p = NULL; uint32_t l = 0;
        if (!e || sysvol_blob(e, &p, &l) != 0) return NULL;
        *len = l; return p;
    }
    if (t->src == SRC_VFS) {
        /* read on demand into vfs_buf */
        if (!t->vfs_buf) {
            int sz = vfs_file_size_in(t->vfs_dir, t->path);
            if (sz <= 0 || sz > (1 << 22)) return NULL;
            t->vfs_buf = (uint8_t *)kmalloc(sz);
            if (!t->vfs_buf) return NULL;
            int got = vfs_read_raw_full_in(t->vfs_dir, t->path, t->vfs_buf, sz);
            if (got <= 0) { kfree(t->vfs_buf); t->vfs_buf = NULL; return NULL; }
            t->data_len = (uint32_t)got;
        }
        *len = t->data_len; return t->vfs_buf;
    }
    return NULL;
}

/* ---------------- WAV decode ---------------- */
/* supports PCM fmt 1, 8/16-bit, mono/stereo, any rate -> we set AC97 rate.
 * converts to stereo s16 interleaved at native rate. */

static int wav_parse(Track *t) {
    uint32_t len; const uint8_t *d = track_data(t, &len);
    if (!d || len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    uint32_t pos = 12;
    int got_fmt = 0;
    while (pos + 8 <= len) {
        uint32_t csz = rd32(d + pos + 4);
        const uint8_t *c = d + pos + 8;
        if (pos + 8 + csz > len) break;
        if (!memcmp(c - 8, "fmt ", 4)) {
            uint16_t fmt = rd16(c), ch = rd16(c + 2);
            t->wav_rate = rd32(c + 4);
            t->wav_bits = rd16(c + 14);
            if (fmt != 1 || (t->wav_bits != 8 && t->wav_bits != 16) || !ch || ch > 2)
                return -1;
            t->wav_ch = ch;
            got_fmt = 1;
        } else if (!memcmp(c - 8, "data", 4)) {
            t->wav_data_off = pos + 8;
            t->wav_data_len = csz;
        }
        pos += 8 + csz + (csz & 1);
    }
    if (!got_fmt || !t->wav_data_off) return -1;
    /* bound data len to file */
    uint32_t avail = len - t->wav_data_off;
    if (t->wav_data_len > avail) t->wav_data_len = avail;
    t->out_rate = t->wav_rate;
    /* total duration estimate */
    uint32_t frame_bytes = t->wav_ch * (t->wav_bits / 8);
    uint32_t frames = frame_bytes ? t->wav_data_len / frame_bytes : 0;
    t->total_ms = t->wav_rate ? (uint64_t)frames * 1000 / t->wav_rate : 0;
    return 0;
}

/* decode next `frames` stereo frames from wav into out (must hold frames*2 s16).
 * returns frames produced. pos tracks byte offset in data chunk. */
static uint32_t wav_decode(Track *t, int16_t *out, uint32_t frames) {
    uint32_t len; const uint8_t *d = track_data(t, &len);
    if (!d) return 0;
    const uint8_t *p = d + t->wav_data_off + t->pos;
    uint32_t end = t->wav_data_off + t->wav_data_len;
    uint32_t avail_frames = (end - (t->wav_data_off + t->pos)) /
                            (t->wav_ch * (t->wav_bits / 8));
    if (frames > avail_frames) frames = avail_frames;
    uint32_t produced = 0;
    if (t->wav_bits == 16) {
        for (uint32_t i = 0; i < frames; i++) {
            int16_t l, r;
            if (t->wav_ch == 2) {
                l = (int16_t)rd16(p); r = (int16_t)rd16(p + 2); p += 4;
            } else {
                l = r = (int16_t)rd16(p); p += 2;
            }
            out[i * 2] = l; out[i * 2 + 1] = r;
            produced++;
        }
    } else { /* 8-bit unsigned */
        for (uint32_t i = 0; i < frames; i++) {
            int16_t l = (int16_t)((p[0] - 128) * 256);
            int16_t r = t->wav_ch == 2 ? (int16_t)((p[1] - 128) * 256) : l;
            p += t->wav_ch;
            out[i * 2] = l; out[i * 2 + 1] = r;
            produced++;
        }
    }
    t->pos += produced * t->wav_ch * (t->wav_bits / 8);
    t->elapsed_ms += (uint64_t)produced * 1000 / t->out_rate;
    return produced;
}

/* ---------------- MP3 decode (minimp3) ---------------- */

/* Exact-ish MP3 duration: prefer the Xing/Info VBR header in the first
 * frame (frame count -> exact length); fall back to a CBR estimate from
 * the probed frame's bitrate. */
static uint64_t mp3_duration_ms(const uint8_t *d, uint32_t len,
                                uint32_t off, uint32_t f0,
                                mp3dec_frame_info_t *fi) {
    /* --- try Xing/Info --- */
    if (f0 + 4 < len) {
        const uint8_t *h = d + f0;
        if ((h[0] & 0xE0) == 0xE0 && (h[0] & 0x06) != 0) {
            int version = (h[0] >> 3) & 3;      /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
            int layer   = (h[0] >> 1) & 3;      /* 1=L3, 2=L2, 3=L1 */
            int protect = h[0] & 1;             /* 0 -> CRC follows header */
            int mode    = (h[3] >> 6) & 3;      /* 3 = mono */
            int mpeg1   = (version == 3);
            int spf     = (layer == 1) ? (mpeg1 ? 1152 : 576)
                                       : (layer == 3 ? 384 : 1152);
            uint32_t side = (uint32_t)(mpeg1 ? (mode == 3 ? 17 : 32) : 17)
                            + (protect ? 0 : 2);
            uint32_t x = f0 + 4 + side;
            if (x + 12 <= len &&
                (!memcmp(d + x, "Xing", 4) || !memcmp(d + x, "Info", 4))) {
                uint32_t flags = rd32(d + x + 4);
                if (flags & 1) {
                    uint32_t frames = rd32(d + x + 8);
                    if (frames && fi->hz)
                        return (uint64_t)frames * spf * 1000 / fi->hz;
                }
            }
        }
    }
    /* --- CBR estimate from real bitrate --- */
    if (fi->bitrate_kbps > 0 && len > off) {
        uint64_t ms = (uint64_t)(len - off) * 8 / fi->bitrate_kbps;
        /* bytes*8 bits / kbps*1000 bit/s -> s; *1000 -> ms cancels */
        if (ms > 0 && ms < 7200000) return ms;
    }
    uint64_t ms = (uint64_t)len * 8 / 32000;
    return ms > 0 ? ms : 60000;
}

static int mp3_parse(Track *t) {
    uint32_t len; const uint8_t *d = track_data(t, &len);
    if (!d || len < 16) return -1;
    /* skip ID3v2 tag if present */
    uint32_t off = 0;
    if (len > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        uint32_t sz = ((d[6] & 0x7f) << 21) | ((d[7] & 0x7f) << 14) |
                      ((d[8] & 0x7f) << 7) | (d[9] & 0x7f);
        off = 10 + sz;
        if (off >= len) return -1;
    }
    /* also skip ID3v1 at end implicitly - decoder handles frames only */
    mp3dec_init(&t->mp3dec);
    t->pos = off;
    /* probe for the first frame that actually yields samples - VBR files
     * often lead with a Xing/Info header frame that decodes to 0 samples,
     * which used to make the whole track get skipped */
    mp3dec_frame_info_t fi;
    int16_t probe[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    int ns = 0;
    uint32_t p = off, f0 = off;
    for (int tries = 0; tries < 8 && p + 4 < len; tries++) {
        f0 = p;
        ns = mp3dec_decode_frame(&t->mp3dec, d + p, (int)(len - p), probe, &fi);
        if (fi.frame_bytes <= 0) break;
        p += (uint32_t)fi.frame_bytes;
        if (ns > 0 && fi.hz) break;
    }
    if (ns <= 0 || !fi.hz) return -1;
    /* rewind: re-init so playback starts clean */
    mp3dec_init(&t->mp3dec);
    t->pos = off;
    t->mp3_rate = fi.hz;
    t->mp3_ch = fi.channels;
    t->out_rate = (uint32_t)fi.hz;
    t->total_ms = mp3_duration_ms(d, len, off, f0, &fi);
    return 0;
}

/* decode next chunk of mp3 -> stereo s16 frames. returns frames produced. */
static uint32_t mp3_decode(Track *t, int16_t *out, uint32_t max_frames) {
    uint32_t len; const uint8_t *d = track_data(t, &len);
    if (!d) return 0;
    static int16_t raw[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    uint32_t produced = 0;
    while (produced < max_frames && t->pos + 4 < len) {
        mp3dec_frame_info_t fi;
        int ns = mp3dec_decode_frame(&t->mp3dec, d + t->pos, (int)(len - t->pos),
                                     raw, &fi);
        if (fi.frame_bytes <= 0) { t->pos = len; break; }
        t->pos += (uint32_t)fi.frame_bytes;
        if (ns <= 0) continue;
        /* ns = samples per channel; raw is interleaved nch channels */
        if (fi.channels == 2) {
            uint32_t n = (uint32_t)ns;
            if (produced + n > max_frames) n = max_frames - produced;
            memcpy(out + produced * 2, raw, n * 4);
            produced += n;
        } else {
            uint32_t n = (uint32_t)ns;
            if (produced + n > max_frames) n = max_frames - produced;
            for (uint32_t i = 0; i < n; i++) {
                out[(produced + i) * 2] = raw[i];
                out[(produced + i) * 2 + 1] = raw[i];
            }
            produced += n;
        }
        t->out_rate = (uint32_t)fi.hz;
        t->elapsed_ms += (uint64_t)ns * 1000 / (fi.hz ? fi.hz : 44100);
    }
    return produced;
}

/* decode `frames` stereo frames from current track into out */
static uint32_t track_decode(Track *t, int16_t *out, uint32_t frames) {
    if (t->kind == 1) return wav_decode(t, out, frames);
    if (t->kind == 2) return mp3_decode(t, out, frames);
    return 0;
}

/* Catmull-Rom cubic resample stereo s16 in_rate -> out_rate.
 * VirtualBox's AC97 VRA accepts non-48k rate writes but its actual
 * sample clock doesn't track them -> everything played slow/weird.
 * The DAC now stays locked at 48000 (the known-good path the chime
 * uses) and we resample here instead. Cubic >> linear: no imaging,
 * no "echo", no metallic edge on transients. */
static inline int16_t cr_sample(int32_t p0, int32_t p1, int32_t p2,
                                int32_t p3, uint32_t t) {
    /* t in Q16 (0..65535); fixed-point Catmull-Rom, result Q16->s16 */
    int64_t tt = t;
    int64_t t2 = (tt * tt) >> 16;
    int64_t t3 = (t2 * tt) >> 16;
    int64_t v = 2 * p1 + ((p2 - p0) * tt >> 16) +
                ((2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 >> 16) +
                ((-p0 + 3 * p1 - 3 * p2 + p3) * t3 >> 16);
    v >>= 1;                                   /* CR has a /2 factor */
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static uint32_t resample_to(const int16_t *in, uint32_t nin, int16_t *out,
                            uint32_t outcap, uint32_t in_rate, uint32_t out_rate) {
    if (!in_rate) in_rate = out_rate;
    if (!out_rate) out_rate = in_rate;
    if (in_rate == out_rate) {
        uint32_t n = nin < outcap ? nin : outcap;
        if (n) memcpy(out, in, (size_t)n * 4);
        return n;
    }
    uint64_t step = ((uint64_t)in_rate << 16) / out_rate;   /* Q16 src/out */
    uint64_t pos = 0;
    uint32_t nout = 0;
    while (nout < outcap) {
        uint32_t i = (uint32_t)(pos >> 16);
        if (i + 1 >= nin) break;
        uint32_t fr = (uint32_t)(pos & 0xFFFFu);
        int32_t l_m = in[(i ? i - 1 : 0) * 2],     r_m = in[(i ? i - 1 : 0) * 2 + 1];
        int32_t l_0 = in[i * 2],                 r_0 = in[i * 2 + 1];
        int32_t l_1 = in[(i + 1) * 2],           r_1 = in[(i + 1) * 2 + 1];
        uint32_t i2 = i + 2 < nin ? i + 2 : nin - 1;
        int32_t l_p = in[i2 * 2],               r_p = in[i2 * 2 + 1];
        out[nout * 2]     = cr_sample(l_m, l_0, l_1, l_p, fr);
        out[nout * 2 + 1] = cr_sample(r_m, r_0, r_1, r_p, fr);
        nout++;
        pos += step;
    }
    return nout;
}

static int track_load(Track *t) {
    memset(t, 0, sizeof(*t));
    t->kind = 0;
    return 0;
}

/* ---------------- app ---------------- */

static void *wmp_create(void) {
    WmpCtx *c = (WmpCtx *)kzalloc(sizeof(WmpCtx));
    /* build playlist: sysvol audio under /music + root audio files,
     * plus VFS user audio files */
    c->pcm = (int16_t *)kmalloc(16384 * 2 * 2);  /* 16384 stereo frames chunk */
    c->pcm2 = (int16_t *)kmalloc(32768 * 2 * 2); /* resampled @48k headroom */
    c->rate = ac97_rate() ? ac97_rate() : 48000;
    int n = 0;
    /* sysvol: scan "music" dir then root for audio extensions. Use the
     * FULL SysVolEnt - VfsEntry.name is truncated to 24 chars which cuts
     * the ".mp3" off long filenames (Roblox/Closer tracks vanished). */
    for (int pass = 0; pass < 2 && n < MAX_TRACKS; pass++) {
        const char *dir = pass == 0 ? "music" : "";
        int cnt = sysvol_count(dir);
        for (int i = 0; i < cnt && n < MAX_TRACKS; i++) {
            const SysVolEnt *se = sysvol_ent(dir, i);
            if (!se || se->v.type != VFS_FILE || !is_audio_name(se->path)) continue;
            Track *t = &c->tracks[n];
            memset(t, 0, sizeof(*t));
            t->src = SRC_SYSVOL;
            snprintf(t->path, sizeof(t->path), "%s", se->path);
            /* display name = basename of the full path (24-char VfsEntry
             * names lose the extension on long filenames) */
            const char *base = strrchr(se->path, '/');
            base = base ? base + 1 : se->path;
            snprintf(t->name, sizeof(t->name), "%s", base);
            char *dot = strrchr(t->name, '.');
            if (dot) *dot = 0;
            if (strstr(t->path, ".mp3") || strstr(t->path, ".MP3")) {
                if (mp3_parse(t) == 0) t->kind = 2;
            } else if (wav_parse(t) == 0) t->kind = 1;
            if (t->kind) n++;
        }
    }
    /* VFS user files: desktop root + inside the user's own folder, so
     * mp3s/wavs dropped into the account folder show up too */
    int vcnt = vfs_count();
    for (int i = 0; i < vcnt && n < MAX_TRACKS; i++) {
        const VfsEntry *e = vfs_entry(i);
        if (!e || e->type != VFS_FILE || !is_audio_name(e->name)) continue;
        Track *t = &c->tracks[n];
        memset(t, 0, sizeof(*t));
        t->src = SRC_VFS;
        snprintf(t->path, sizeof(t->path), "%s", e->name);
        snprintf(t->name, sizeof(t->name), "%s", e->name);
        char *dot = strrchr(t->name, '.');
        if (dot) *dot = 0;
        if (strstr(t->path, ".mp3") || strstr(t->path, ".MP3")) {
            if (mp3_parse(t) == 0) t->kind = 2;
        } else if (wav_parse(t) == 0) t->kind = 1;
        if (t->kind) n++;
    }
    const char *uf = gui_user_name();
    if (uf && uf[0] && vfs_find(uf) >= 0) {
        int ucnt = vfs_count_in(uf);
        for (int i = 0; i < ucnt && n < MAX_TRACKS; i++) {
            const VfsEntry *e = vfs_entry_in(uf, i);
            if (!e || e->type != VFS_FILE || !is_audio_name(e->name)) continue;
            Track *t = &c->tracks[n];
            memset(t, 0, sizeof(*t));
            t->src = SRC_VFS;
            strncpy(t->vfs_dir, uf, sizeof(t->vfs_dir) - 1);
            snprintf(t->path, sizeof(t->path), "%s", e->name);
            snprintf(t->name, sizeof(t->name), "%s", e->name);
            char *dot = strrchr(t->name, '.');
            if (dot) *dot = 0;
            if (strstr(t->path, ".mp3") || strstr(t->path, ".MP3")) {
                if (mp3_parse(t) == 0) t->kind = 2;
            } else if (wav_parse(t) == 0) t->kind = 1;
            if (t->kind) n++;
        }
    }
    c->ntracks = n;
    c->track = 0; c->sel = 0;
    c->sbar = -1;
    c->hov_btn = c->hov_row = c->hov_sb = -1;
    c->prs_btn = c->prs_row = -1;
    kprintf("wmp: %d tracks\n", n);
    for (int i = 0; i < n && i < 8; i++)
        kprintf("wmp:  [%d] %s kind=%d rate=%u len=%u\n", i,
                c->tracks[i].name, c->tracks[i].kind,
                c->tracks[i].out_rate, c->tracks[i].data_len);
    if (n == 0) {
        /* no audio files - add a placeholder so UI isn't empty */
        snprintf(c->tracks[0].name, MAX_NAME, "(no audio files)");
        c->ntracks = 1;
    }
    return c;
}

/* only ONE player may feed the DAC at a time - two instances writing
 * interleaved chunks to the same ring is exactly the "dundun dundudnu"
 * doubled-echo the user heard. Claim on play; others auto-stop. */
static WmpCtx *g_audio_owner;

static void wmp_destroy(void *ctx) {
    WmpCtx *c = (WmpCtx *)ctx;
    if (g_audio_owner == c) g_audio_owner = NULL;
    ac97_release(c);
    ac97_flush();
    for (int i = 0; i < c->ntracks; i++)
        if (c->tracks[i].vfs_buf) kfree(c->tracks[i].vfs_buf);
    if (c->pcm) kfree(c->pcm);
    if (c->pcm2) kfree(c->pcm2);
    kfree(c);
}
static void wmp_select(WmpCtx *c, int i);
static void wmp_play(WmpCtx *c);

static void wmp_init(void *ctx) {
    WmpCtx *c = (WmpCtx *)ctx;
    /* double-clicked an audio file? jump straight to it and play */
    const char *pa = gui_pending_audio();
    if (pa && pa[0]) {
        for (int i = 0; i < c->ntracks; i++) {
            char full[160];
            if (c->tracks[i].vfs_dir[0])
                snprintf(full, sizeof(full), "%s/%s",
                         c->tracks[i].vfs_dir, c->tracks[i].path);
            else full[0] = 0;
            if (!strcmp(c->tracks[i].path, pa) ||
                !strcmp(c->tracks[i].name, pa) ||
                (full[0] && !strcmp(full, pa))) {
                wmp_select(c, i);
                wmp_play(c);
                break;
            }
        }
        gui_clear_pending_audio();
    }
}

/* minimal layout: song name + time / transport / song list / visualiser.
 * playlist geometry is stored in ctx each draw for hit-testing + wheel. */
#define BTN_Y 36
#define BTN_H 30
#define BTN_N 4                 /* |<  >  []  >|  */
#define BTN_GAP 8
#define PL_LABEL_Y 80
#define PL_Y 92
#define PL_ROW 20
/* visualiser height is dynamic: bottom third of the window, clamped */
static int viz_height(int H) {
    int v = H / 3;
    if (v < 84) v = 84;
    if (v > 128) v = 128;
    return v;
}

static void wmp_draw(void *ctx) {
    WmpCtx *c = (WmpCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(39, 13, 42));
    app_fill(0, 0, W, 24, rgb(53, 20, 58));
    app_fill(0, 24, W, 1, rgb(150, 70, 170));

    Track *t = &c->tracks[c->track];

    /* LCD shows elapsed / time-LEFT: left counts up, right counts
     * down from the track's real duration so it reads like a player. */
    char tbuf[48];
    uint64_t el = t->elapsed_ms, tot = t->total_ms;
    if (c->playing && !c->paused) {
        uint32_t q = ac97_level();
        /* + resampled PCM still pending in userspace (not yet in the ring) */
        q += (uint32_t)((c->rframes - c->rpos) * 1000 / c->rate);
        el = el > q ? el - q : 0;
    }
    if (el > tot && tot) el = tot;
    uint64_t rem = tot > el ? tot - el : 0;
    snprintf(tbuf, sizeof(tbuf), "%u:%02u / %u:%02u",
             (unsigned)(el / 60000), (unsigned)((el / 1000) % 60),
             (unsigned)(rem / 60000), (unsigned)((rem / 1000) % 60));
    /* freestanding snprintf lacks the '0' flag - force 2-digit fields */
    {
        uint64_t e2 = el, t2 = rem;
        if (e2 > 3599999) e2 = 3599999; if (t2 > 3599999) t2 = 3599999;
        int em = (int)(e2 / 60000), es = (int)((e2 / 1000) % 60);
        int tm = (int)(t2 / 60000), ts = (int)((t2 / 1000) % 60);
        char *p = tbuf;
        *p++ = (char)('0' + em / 10); *p++ = (char)('0' + em % 10); *p++ = ':';
        *p++ = (char)('0' + es / 10); *p++ = (char)('0' + es % 10);
        *p++ = ' '; *p++ = '/'; *p++ = ' ';
        *p++ = (char)('0' + tm / 10); *p++ = (char)('0' + tm % 10); *p++ = ':';
        *p++ = (char)('0' + ts / 10); *p++ = (char)('0' + ts % 10);
        *p = 0;
    }
    int tw = app_text_width(tbuf, 1);
    app_text(W - tw - 12, 7, tbuf, rgb(140, 245, 225), 1);
    /* long song names MARQUEE-scroll left instead of clipping dead:
     * slides ~25px/s, pauses, snaps back - like a real player title */
    {
        int avail = W - tw - 48;          /* px between x=12 and timer */
        int name_w = app_text_width(t->name, 1);
        int dx = 12;
        if (name_w > avail && avail > 40) {
            int travel = name_w - avail + 30;
            int cyc = travel + 140;       /* ~3.5s loop at /40ms ticks */
            int p = (int)((gui_ticks() / 40) % cyc);
            if (p <= travel) dx = 12 - p;
            else dx = 12 - travel;        /* hold on the tail, then wrap */
        }
        app_text_clip(dx, 7, t->name, rgb(250, 235, 255), 1,
                      8, 2, avail + 4, 18);
    }

    /* transport: prev / play-pause / stop / next */
    int btw = (W - 20 - BTN_GAP * (BTN_N - 1)) / BTN_N;
    static const char *lbls[BTN_N] = { "|<", ">", "[]", ">|" };
    for (int i = 0; i < BTN_N; i++) {
        int bx = 10 + i * (btw + BTN_GAP);
        bool hot = (i == 1) && c->playing && !c->paused;
        bool hov = (c->hov_btn == i);
        bool prs = (c->prs_btn == i);
        app_rounded(bx, BTN_Y, btw, BTN_H, 6,
                    prs ? rgb(67, 28, 78) :
                    hov ? rgb(154, 72, 190) :
                    hot ? rgb(92, 40, 140) : rgb(99, 46, 108));
        app_rect(bx, BTN_Y, btw, BTN_H,
                 prs ? rgb(162, 90, 180) :
                 hov ? rgb(227, 160, 255) : rgb(202, 110, 220));
        const char *s = lbls[i];
        if (i == 1) s = (c->playing && !c->paused) ? "||" : ">";
        int tw2 = app_text_width(s, 1);
        app_text(bx + (btw - tw2) / 2, BTN_Y + 11, s, rgb(250, 235, 255), 1);
    }

    /* song list: fills everything down to the visualiser */
    int viz_h = viz_height(H);
    int pl_bot = H - viz_h - 4;
    int vis = (pl_bot - PL_Y) / PL_ROW;
    if (vis < 2) vis = 2;
    app_text(12, PL_LABEL_Y, "Songs", rgb(248, 228, 255), 1);
    app_fill(10, PL_Y - 4, W - 20, 1, rgb(126, 60, 140));
    /* persistent scroll offset (wheel), clamped + keeps selection visible */
    int maxsc = c->ntracks - vis; if (maxsc < 0) maxsc = 0;
    if (c->scroll > maxsc) c->scroll = maxsc;
    if (c->scroll < 0) c->scroll = 0;
    if (c->sel < c->scroll) c->scroll = c->sel;
    if (c->sel >= c->scroll + vis) c->scroll = c->sel - vis + 1;
    c->pl_vis = vis; c->pl_scroll = c->scroll; c->pl_top = PL_Y;
    for (int i = 0; i < vis; i++) {
        int ti = c->scroll + i;
        if (ti >= c->ntracks) break;
        int iy = PL_Y + i * PL_ROW;
        bool sel = (c->sel == ti);
        bool hov = (c->hov_row == ti);
        bool prs = (c->prs_row == ti);
        if (sel) app_fill(10, iy, W - 20, PL_ROW - 3, rgba(150, 70, 170, 210));
        else if (prs) app_fill(10, iy, W - 20, PL_ROW - 3, rgba(105, 50, 120, 190));
        else if (hov) app_fill(10, iy, W - 20, PL_ROW - 3, rgba(150, 70, 170, 110));
        app_rect(10, iy, W - 20, PL_ROW - 3,
                 prs ? rgb(132, 60, 150) :
                 hov ? rgb(217, 140, 240) : rgb(182, 96, 200));
        app_text_clip(16, iy + 5, c->tracks[ti].name,
                      sel ? rgb(255, 255, 255) : rgb(234, 205, 240), 1,
                      12, iy + 1, W - 64, PL_ROW - 5);
        if (ti == c->track && c->playing)
            app_text(W - 40, iy + 5, ">>", rgb(120, 230, 190), 1);
    }
    /* scrollbar when the list overflows - draggable thumb like Finder's.
     * Geometry is stored every frame so the event handler can hit-test
     * and drag it (a visible-but-dead bar was the bug). */
    {
        int sh = vis * vis / (c->ntracks ? c->ntracks : 1);
        if (sh < 12) sh = 12;
        if (sh > pl_bot - PL_Y) sh = pl_bot - PL_Y;
        int sy = PL_Y + (pl_bot - PL_Y - sh) * c->scroll / (maxsc ? maxsc : 1);
        if (c->ntracks > vis)
            app_fill(W - 10, sy, 6, sh, rgb(172, 90, 190));
        c->pl_bot = pl_bot;
        c->sb_tx = W - 10; c->sb_ty = PL_Y; c->sb_th = pl_bot - PL_Y;
        c->sb_my = sy; c->sb_mh = sh; c->sb_maxsc = maxsc;
    }

    /* hover feedback: which transport button / playlist row / scrollbar
     * the pointer is over - drives highlight + hand cursor */
    {
        int mx = c->mx, my = c->my;
        c->hov_btn = c->hov_row = c->hov_sb = -1;
        int btw2 = (W - 20 - BTN_GAP * (BTN_N - 1)) / BTN_N;
        for (int i = 0; i < BTN_N; i++) {
            int bx = 10 + i * (btw2 + BTN_GAP);
            if (mx >= bx && mx < bx + btw2 && my >= BTN_Y && my < BTN_Y + BTN_H)
                c->hov_btn = i;
        }
        if (c->hov_btn < 0 && c->ntracks > vis && c->sb_maxsc > 0 &&
            mx >= W - 10 && mx < W - 4 && my >= PL_Y && my < pl_bot)
            c->hov_sb = 1;
        if (c->hov_btn < 0 && c->hov_sb < 0) {
            for (int i = 0; i < vis; i++) {
                int ti = c->scroll + i;
                if (ti >= c->ntracks) break;
                int iy = PL_Y + i * PL_ROW;
                if (mx >= 10 && mx < W - 10 && my >= iy && my < iy + PL_ROW - 3) {
                    c->hov_row = ti; break;
                }
            }
        }
        static int last_hov = -2;
        int hov = (c->hov_btn >= 0 || c->hov_row >= 0 || c->hov_sb >= 0) ? 1 : 0;
        if (hov != last_hov) { last_hov = hov; gui_set_cursor(hov ? CUR_HAND : -1); }
    }

    /* visualiser: REAL spectrum - but ONLY of THIS player's stream.
     * ac97_spectrum reads whatever is in the DMA ring; gating on
     * ownership+playing keeps chimes/beeps/system sounds out of it.
     * Instant attack, ~0.4s release so bars dance with the rhythm. */
    int vy = pl_bot + 3;
    int vh = H - vy - 4;
    if (vh < 30) vh = 30;
    uint8_t raw[AC97_SPEC_BANDS];
    if (g_audio_owner == c && c->playing && !c->paused) {
        ac97_spectrum(raw);
    } else {
        memset(raw, 0, sizeof(raw));   /* not ours: bars decay flat */
    }
    for (int i = 0; i < AC97_SPEC_BANDS; i++) {
        if (raw[i] > c->viz[i]) c->viz[i] = raw[i];
        else c->viz[i] -= (c->viz[i] >> 4) + 1;
        if (c->viz[i] < 0) c->viz[i] = 0;
    }
    int nbars = AC97_SPEC_BANDS;
    int step = (W - 20) / nbars;
    for (int i = 0; i < nbars; i++) {
        int h = 4 + c->viz[i] * (vh - 14) / 255;
        if (h > vh - 10) h = vh - 10;
        /* bass->treble gradient: cyan lows to violet highs */
        int rr = 90 + i * 8, gg = 215 - i * 4, bb = 255 - i * 6;
        if (rr > 255) rr = 255;
        app_fill(10 + i * step + 1, vy + vh - 6 - h, step - 3, h,
                 rgb(rr, gg, bb));
    }
    /* baseline under the bars */
    app_fill(10, vy + vh - 5, W - 20, 1, rgb(126, 60, 140));
}

/* reset decode state for track t and set AC97 rate */
/* full select: resets decode + flushes the DAC. Internal use only -
 * UI paths go through the guarded wmp_select below. */
static void wmp_select_reset(WmpCtx *c, int i) {
    if (i < 0 || i >= c->ntracks) return;
    c->track = i; c->sel = i;
    Track *t = &c->tracks[i];
    /* reset decode position */
    if (t->kind == 2) { mp3dec_init(&t->mp3dec); }
    /* re-seek mp3 to after ID3 */
    if (t->kind == 2) {
        uint32_t len; const uint8_t *d = track_data(t, &len);
        uint32_t off = 0;
        if (d && len > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
            uint32_t sz = ((d[6] & 0x7f) << 21) | ((d[7] & 0x7f) << 14) |
                          ((d[8] & 0x7f) << 7) | (d[9] & 0x7f);
            off = 10 + sz;
        }
        t->pos = off;
    } else {
        t->pos = 0; t->elapsed_ms = 0;
    }
    t->elapsed_ms = 0;
    /* flush whatever the DAC still has queued at the OLD rate, then pin
     * the output to 48000 - VBox's AC97 VRA accepts 22k/44.1k writes but
     * its real sample clock doesn't follow -> slow/pitch-bent audio.
     * Everything is cubic-resampled to 48k instead (known-good path). */
    ac97_flush();
    ac97_set_rate(48000);
    c->rate = 48000;
    c->pcm_frames = 0; c->pcm_pos = 0;
    c->rframes = 0; c->rpos = 0;
    kprintf("wmp: play '%s' kind=%d in_rate=%u dac=%u\n", t->name, t->kind,
            t->out_rate, c->rate);
}

/* UI-facing select: re-selecting the song already playing must NOT
 * flush+restart it (stray clicks / double-click tails were the "dundun"
 * glitch). Different track still switches normally. */
static void wmp_select(WmpCtx *c, int i) {
    if (i == c->track && c->playing && !c->paused) { c->sel = i; return; }
    wmp_select_reset(c, i);
}

static void wmp_play(WmpCtx *c) {
    if (!c->playing) {
        c->playing = 1; c->paused = 0;
        g_audio_owner = c;              /* take over the DAC */
        ac97_claim(c);                  /* blocks UI beeps mid-song */
        wmp_select_reset(c, c->track);  /* always restart from top */
        c->tick0 = gui_ticks();
    } else if (c->paused) {
        c->paused = 0;
    } else {
        /* PAUSE = instant silence: flush the ~800ms DMA ring + drop the
         * pending resampled chunk so nothing keeps playing. Decode
         * position stays put - resume continues from there (the flushed
         * tail is skipped, <1s jump, vs. the old ~1s lag before silence). */
        c->paused = 1;
        ac97_flush();
        c->rpos = c->rframes;
    }
}
static void wmp_stop(WmpCtx *c) {
    c->playing = 0; c->paused = 0;
    if (g_audio_owner == c) g_audio_owner = NULL;
    ac97_release(c);
    ac97_flush();
    c->rpos = c->rframes = 0;   /* drop pending PCM - stop is instant */
    wmp_select_reset(c, c->track);
}

static void wmp_event(void *ctx, GuiEvent *ev) {
    WmpCtx *c = (WmpCtx *)ctx;
    if (ev->type == EV_MOUSE_MOVE) { c->mx = ev->x; c->my = ev->y; }
    /* scrollbar thumb drag has priority over everything else */
    if (ev->type == EV_MOUSE_MOVE && c->sbar >= 0) {
        int travel = c->sb_th - c->sb_mh;
        if (travel > 0 && c->sb_maxsc > 0) {
            int ns = (ev->y - c->sb_ty - c->sbar) * c->sb_maxsc / travel;
            if (ns < 0) ns = 0;
            if (ns > c->sb_maxsc) ns = c->sb_maxsc;
            c->scroll = ns;
        }
        return;
    }
    if (ev->type == EV_MOUSE_UP) {
        c->sbar = -1; c->prs_btn = -1; c->prs_row = -1; return;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int x = ev->x, y = ev->y;
        int W = app_content_w();
        c->prs_btn = c->prs_row = -1;
        /* playlist scrollbar: grab thumb, or click track to page -
         * checked first because rows extend under the bar's x-range */
        if (c->sb_maxsc > 0 && c->sb_th > c->sb_mh &&
            x >= c->sb_tx && x < c->sb_tx + 6 &&
            y >= c->sb_ty && y < c->sb_ty + c->sb_th) {
            if (y >= c->sb_my && y < c->sb_my + c->sb_mh)
                c->sbar = y - c->sb_my;                    /* grab thumb */
            else {
                int page = c->sb_th * 3 / 4;
                c->scroll += (y < c->sb_my) ? -page : page;
                if (c->scroll < 0) c->scroll = 0;
                if (c->scroll > c->sb_maxsc) c->scroll = c->sb_maxsc;
            }
            return;
        }
        int btw = (W - 20 - BTN_GAP * (BTN_N - 1)) / BTN_N;
        for (int i = 0; i < BTN_N; i++) {
            int bx = 10 + i * (btw + BTN_GAP);
            if (x >= bx && x < bx + btw && y >= BTN_Y && y < BTN_Y + BTN_H) {
                c->prs_btn = i;
                if (i == 0) { wmp_select(c, (c->track + c->ntracks - 1) % c->ntracks); wmp_play(c); }
                else if (i == 1) wmp_play(c);              /* play / pause */
                else if (i == 2) wmp_stop(c);              /* stop */
                else { wmp_select(c, (c->track + 1) % c->ntracks); wmp_play(c); }
                return;
            }
        }
        /* playlist rows (geometry stored by draw this frame) */
        for (int i = 0; i < c->pl_vis; i++) {
            int ti = c->pl_scroll + i;
            if (ti >= c->ntracks) break;
            int iy = c->pl_top + i * PL_ROW;
            if (x >= 10 && x < W - 10 && y >= iy && y < iy + PL_ROW - 3) {
                c->prs_row = ti;
                wmp_select(c, ti);
                c->scroll = c->pl_scroll;   /* keep list put on click-select */
                return;
            }
        }
    }
    /* mouse wheel scrolls the song list one row per notch
     * (wheel events don't carry content coords, so no hit-test) */
    if (ev->type == EV_MOUSE_WHEEL) {
        c->scroll -= ev->wheel;
    }
    if (ev->type == EV_KEY_CHAR) {
        if (ev->key == ' ') { wmp_play(c); return; }
    }
    if (ev->type == EV_KEY_SPECIAL) {
        if (ev->key == KEY_LEFT) { wmp_select(c, (c->track + c->ntracks - 1) % c->ntracks); wmp_play(c); }
        else if (ev->key == KEY_RIGHT) { wmp_select(c, (c->track + 1) % c->ntracks); wmp_play(c); }
        else if (ev->key == KEY_UP) { if (c->sel > 0) c->sel--; }
        else if (ev->key == KEY_DOWN) { if (c->sel < c->ntracks - 1) c->sel++; }
        else if (ev->key == KEY_ENTER) { wmp_select(c, c->sel); wmp_play(c); }
        else if (ev->key == KEY_ESC) wmp_stop(c);
    }
}

/* feed AC97: keep ~300ms of audio queued */
static void wmp_update(void *ctx) {
    WmpCtx *c = (WmpCtx *)ctx;
    if (!c->playing || c->paused) return;
    if (g_audio_owner != c) {           /* another player took the DAC */
        c->playing = 0; c->paused = 0;
        ac97_release(c);
        return;
    }
    Track *t = &c->tracks[c->track];
    if (!t->kind) { wmp_stop(c); return; }

    /* keep the DAC fed: decode ahead only while the buffer is below ~600ms
     * (the driver itself caps the ring ~1.3s ahead of CIV, so the buffer
     * stays deep and underruns - the crackle - can't happen) */
    uint32_t lvl = ac97_level();

    /* decode a chunk if we don't have pending resampled PCM.
     * 9216 frames = exactly 8 MP3 frames, so mp3_decode never clips
     * a partial frame at the chunk boundary (that caused periodic clicks).
     * BIG chunk on purpose: ~209ms of audio per decode. The old 2304-frame
     * chunk only produced ~52ms per decode-cycle while the DAC consumes
     * ~48 frames/ms continuously - at GUI rates below ~45fps production
     * fell behind consumption and the ring starved to zero (the crackle).
     * Serial showed lvl oscillating 6<->590ms = classic underrun cycle. */
    if (c->rpos >= c->rframes) {
        if (lvl > 800) return;
        c->pcm_frames = track_decode(t, c->pcm, 9216);
        c->pcm_pos = 0;
        c->in_rate = t->out_rate ? t->out_rate : c->rate;
        c->rframes = resample_to(c->pcm, c->pcm_frames, c->pcm2, 32768,
                                 c->in_rate, c->rate);
        c->rpos = 0;
        /* TEMP verification: FNV-1a hashes of decoded + resampled PCM.
         * Host-side replica computes the same hashes - mismatch pins the
         * corruption to decode/resample (match = DMA/emulator side). */
        {
            static int hcnt;
            if (hcnt < 60) {
                const uint8_t *pin = (const uint8_t *)c->pcm;
                const uint8_t *pout = (const uint8_t *)c->pcm2;
                uint32_t hi = 2166136261u, ho = 2166136261u;
                for (uint32_t i = 0; i < c->pcm_frames * 4; i++) {
                    hi ^= pin[i]; hi *= 16777619u;
                }
                for (uint32_t i = 0; i < c->rframes * 4; i++) {
                    ho ^= pout[i]; ho *= 16777619u;
                }
                kprintf("wmp: h%u in=%u out=%u hi=%08x ho=%08x\n",
                        hcnt, c->pcm_frames, c->rframes, hi, ho);
                hcnt++;
            }
        }
        static int dbg;
        if (dbg++ < 6) {
            /* peak of the resampled chunk - proves non-silent PCM
             * actually reaches the DAC write path */
            int peak = 0;
            for (uint32_t i = 0; i < c->rframes * 2 && i < 8192; i++) {
                int v = c->pcm2[i] < 0 ? -c->pcm2[i] : c->pcm2[i];
                if (v > peak) peak = v;
            }
            kprintf("wmp: dec=%u in=%u out=%u lvl=%u peak=%d\n", c->pcm_frames,
                    c->in_rate, c->rate, lvl, peak);
        }
        /* PICB snapshot ~every 2s of wall time while playing */
        static uint64_t last_dbg;
        if (gui_ticks() - last_dbg > 2000) {
            last_dbg = gui_ticks();
            ac97_debug();
            ac97_diag();
        }
        if (c->rframes == 0) {
            /* end of track -> advance (reset variant so a 1-track
             * playlist actually loops instead of stalling) */
            wmp_select_reset(c, (c->track + 1) % c->ntracks);
            return;
        }
    }
    /* write remaining resampled PCM to the DAC - ac97_write may accept
     * only part of it when the ring is full; keep the rest pending for
     * the next update (dropping it was the stutter).
     * CAP at 6144 frames/call (~3 descriptors, 128ms of audio): big
     * enough that production always beats the 48kHz consumer even at
     * low GUI frame rates, small enough to avoid committing the whole
     * ring in one burst. ac97_write itself stops ~3 descriptors short
     * of lapping CIV, so the DAC can never catch the write head. */
    uint32_t rem = c->rframes - c->rpos;
    if (rem > 6144) rem = 6144;
    uint32_t w = ac97_write(c->pcm2 + c->rpos * 2, rem);
    c->rpos += w;
}

static App s_app = {
    "Voyage Player", "Voyage Player", 420, 340, -1,
    wmp_create, wmp_destroy, wmp_init, wmp_draw, wmp_event, wmp_update, 0, 0
};
App *make_voyageplayer(void) { return &s_app; }
