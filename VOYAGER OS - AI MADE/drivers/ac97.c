/* AC97 (Intel ICH / QEMU AC97 / VBox "Intel AC97") PCM playback driver.
 * DMA architecture transplanted from the Retro 95 kernel driver, which is
 * proven crackle-free on the same emulated hardware:
 *   - ONE physically-contiguous 256KB PCM pool (our heap arena is itself
 *     physically contiguous, so a single kmalloc is DMA-safe)
 *   - 32 descriptors x 4096 16-bit samples, BUP set on every descriptor
 *   - NON-BLOCKING feed: keeps >=3 descriptors of headroom so the write
 *     cursor can never lap the DAC's read cursor (CIV) - the old driver
 *     busy-spun until the ring was 100% full, so every GUI hiccup became
 *     an underrun = the periodic crackle
 *   - DCH underrun-halt auto-resume on every write/level call
 *   - channel reset (CR.RR) wipes BDBAR/LVI/CIV -> always reprogrammed
 *
 * NAMBAR (BAR0) = codec mixer regs (16-bit), NABMBAR (BAR1) = DMA engine.
 * Polling only - no IRQ needed; the caller paces itself on CIV/LVI. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ac97.h"
#include "../kernel/k.h"
#include "../kernel/arch/arch.h"

/* ---------- port IO ---------- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* ---------- PCI ---------- */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) | (off & 0xFC);
    outl(0xCF8, a);
    return inl(0xCFC);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) | (off & 0xFC);
    outl(0xCF8, a);
    outl(0xCFC, v);
}
static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (uint16_t)(pci_read32(bus, dev, fn, off & 0xFC) >> ((off & 2) * 8));
}
static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) | (off & 0xFC);
    uint32_t cur = pci_read32(bus, dev, fn, off & 0xFC);
    uint32_t sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(bus, dev, fn, off & 0xFC, cur);
    (void)a;
}

/* ---------- state ---------- */
#define AU_NDESC   32             /* AC97 hardware/QEMU limit is 32 entries */
#define AU_BUFSAMP 4096           /* 16-bit samples per descriptor */
                                /* = 2048 stereo frames = ~46ms @44.1k */
#define AU_BD_BUP  0x4000         /* BUP flag in ctrl half of ctl_len */
#define POOL_BYTES ((uint64_t)AU_NDESC * AU_BUFSAMP * 2)

struct au_desc { uint32_t base; uint16_t len; uint16_t ctrl; } __attribute__((packed));

static uint16_t g_nam, g_nabm;
static struct au_desc *g_bd;       /* BDL (physically contiguous) */
static uint64_t     g_bd_phys;
static uint8_t     *g_pool;        /* one contiguous PCM arena */
static uint64_t     g_pool_phys;
static uint32_t     g_cur;         /* descriptor currently being filled */
static uint32_t     g_fillpos;     /* 16-bit samples filled in g_cur */
static int          g_ready;
static uint32_t     g_rate = 48000;
static void        *g_claim;         /* single-producer owner token */

/* NABM absolute offsets */
#define NABM_BDBAR 0x10
#define NABM_CIV   0x14
#define NABM_LVI   0x15
#define NABM_SR    0x16
#define NABM_CR    0x1B
#define NABM_GLB_C 0x2C
#define NABM_GLB_S 0x30

#define PO_CR_RPBM 0x01
#define PO_CR_RR   0x02
#define PO_SR_DCH  0x01

static int bd_civ(void) { return inb(g_nabm + NABM_CIV); }

/* AC97 halts the channel on underrun (DCH in SR) and stays halted even
 * after LVI advances - without this the DAC goes permanently silent.
 * Re-arming RPBM clears DCH and resumes at the current descriptor WITHOUT
 * wiping BDBAR/LVI (unlike CR.RR). Called on every write/level. */
static void ac97_tick(void) {
    if (!g_ready) return;
    uint8_t sr = inb(g_nabm + NABM_SR);
    uint8_t cr = inb(g_nabm + NABM_CR);
    if ((sr & PO_SR_DCH) || !(cr & PO_CR_RPBM))
        outb(g_nabm + NABM_CR, PO_CR_RPBM);
}

int ac97_ready(void) { return g_ready; }
uint32_t ac97_rate(void) { return g_rate; }

/* single-producer lock: only one client may stream at a time. GUI sounds
 * (beeps) check ac97_claimed() so they never inject samples mid-song. */
int ac97_claim(void *owner) {
    if (g_claim && g_claim != owner) return 0;
    g_claim = owner;
    return 1;
}
void ac97_release(void *owner) {
    if (g_claim == owner) g_claim = NULL;
}
int ac97_claimed(void) { return g_claim != NULL; }

/* 0-100 -> AC97 attenuation on master + PCM-out (0 = 0dB, higher = quieter) */
void ac97_set_volume(int pct) {
    if (!g_ready) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int att = (100 - pct) * 31 / 100;      /* 0..31 -> 0dB .. ~-46dB */
    uint16_t v = (uint16_t)((att & 0x1F) | ((att & 0x1F) << 8));
    if (pct == 0) v |= 0x8080;             /* hard mute both channels */
    outw(g_nam + 0x02, v);                 /* master volume */
    outw(g_nam + 0x18, v);                 /* PCM out volume */
}
int ac97_volume(void) {
    if (!g_ready) return 100;
    uint16_t v = inw(g_nam + 0x18);
    if (v & 0x8080) return 0;              /* muted */
    int att = v & 0x1F;
    return 100 - att * 100 / 31;
}

int ac97_init(void) {
    /* scan PCI for a multimedia-audio class device (0x04/0x01) */
    int bus = -1, dev = -1, fn = -1;
    for (int b = 0; b < 256 && bus < 0; b++) {
        for (int d = 0; d < 32 && bus < 0; d++) {
            for (int f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0x00);
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) continue;
                uint32_t cls = pci_read32(b, d, f, 0x08);
                uint8_t base = (uint8_t)(cls >> 24), sub = (uint8_t)(cls >> 16);
                if (base == 0x04 && sub == 0x01) { bus = b; dev = d; fn = f; break; }
            }
        }
    }
    if (bus < 0) { kprintf("ac97: no audio device\n"); return -1; }

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, cmd | 0x5);   /* I/O space + bus master */

    uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
    uint32_t bar1 = pci_read32(bus, dev, fn, 0x14);
    if (!(bar0 & 1) || !(bar1 & 1)) { kprintf("ac97: BARs not I/O\n"); return -1; }
    g_nam = (uint16_t)(bar0 & ~3u);
    g_nabm = (uint16_t)(bar1 & ~3u);

    /* release AC-link cold reset, wait for codec ready (GLOB_STA bit8) */
    outl(g_nabm + NABM_GLB_C, inl(g_nabm + NABM_GLB_C) | 0x02);
    int ok = 0;
    for (int i = 0; i < 1000000; i++)
        if (inl(g_nabm + NABM_GLB_S) & 0x100) { ok = 1; break; }
    if (!ok) kprintf("ac97: codec not ready ( continuing anyway )\n");

    /* codec reset + volumes (0x0000 = 0dB both channels, unmuted) */
    outw(g_nam + 0x00, 0x0000);
    for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause");
    outw(g_nam + 0x02, 0x0000);               /* master volume */
    outw(g_nam + 0x18, 0x0000);               /* PCM out volume */

    /* variable sample rate if the codec supports it */
    uint16_t eaid = inw(g_nam + 0x28);
    if (eaid & 0x0001) {
        outw(g_nam + 0x2a, inw(g_nam + 0x2a) | 0x0001);  /* VRA enable */
        outw(g_nam + 0x2c, (uint16_t)g_rate);
        if (inw(g_nam + 0x2c) == (uint16_t)g_rate)
            kprintf("ac97: VRA ok, rate=%u\n", g_rate);
        else
            kprintf("ac97: VRA rate rejected, fixed 48k\n");
    } else {
        g_rate = 48000;
        kprintf("ac97: no VRA, fixed 48k\n");
    }

    /* BDL + ONE contiguous PCM pool. kmalloc returns runs inside the
     * static heap arena which is physically contiguous, so a single big
     * allocation is DMA-safe (Retro 95 does the same via pmm). */
    g_bd = (struct au_desc *)kmalloc(sizeof(struct au_desc) * AU_NDESC);
    g_pool = (uint8_t *)kmalloc(POOL_BYTES);
    if (!g_bd || !g_pool) { kprintf("ac97: DMA mem alloc failed\n"); return -1; }
    g_bd_phys  = kvirt_to_phys(g_bd);
    g_pool_phys = kvirt_to_phys(g_pool);
    if (g_bd_phys > 0xFFFFFFFFull || g_pool_phys > 0xFFFFFFFFull ||
        g_pool_phys + POOL_BYTES > 0x100000000ull) {
        kprintf("ac97: phys>4G\n"); return -1;
    }
    memset(g_pool, 0, POOL_BYTES);
    for (int i = 0; i < AU_NDESC; i++) {
        g_bd[i].base = (uint32_t)(g_pool_phys + (uint64_t)i * AU_BUFSAMP * 2);
        g_bd[i].len  = AU_BUFSAMP;
        g_bd[i].ctrl = AU_BD_BUP;      /* BUP: repeat last sample on underrun */
    }

    /* channel reset FIRST (it wipes BDBAR/LVI/CIV/PICB), then program */
    outb(g_nabm + NABM_CR, PO_CR_RR);
    for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause");
    outl(g_nabm + NABM_BDBAR, (uint32_t)g_bd_phys);
    g_cur = 0; g_fillpos = 0;   /* ALL 32 descriptors carry audio (Retro 95
                                 * layout). A reserved silence BD0 broke the
                                 * LVI sequence: when the write head lapped
                                 * through index 0, LVI collapsed to 0 and the
                                 * emulator halted/skipped descriptors -
                                 * the block-level scramble in captures. */
    outb(g_nabm + NABM_LVI, 0);
    outb(g_nabm + NABM_CR, PO_CR_RPBM);   /* run: pool is zeroed = silence */

    g_ready = 1;
    kprintf("ac97: ready nam=%x nabm=%x rate=%u\n", g_nam, g_nabm, g_rate);
    return 0;
}

int ac97_set_rate(uint32_t hz) {
    if (!g_ready) return -1;
    if (hz < 4000) hz = 4000;
    if (hz > 48000) hz = 48000;
    uint16_t eaid = inw(g_nam + 0x28);
    if (!(eaid & 0x0001)) { g_rate = 48000; return (hz == 48000) ? 0 : -1; }
    outw(g_nam + 0x2a, inw(g_nam + 0x2a) | 0x0001);
    outw(g_nam + 0x2c, (uint16_t)hz);
    if (inw(g_nam + 0x2c) != (uint16_t)hz) return -1;
    g_rate = hz;
    return 0;
}

/* Feed interleaved stereo frames. NON-BLOCKING (Retro 95 flow control):
 * stops when >=3 descriptors are queued ahead of the DAC's CIV, so the
 * ring always keeps write headroom and can never lap the read cursor.
 * Returns frames actually queued - caller retries the rest next tick. */
uint32_t ac97_write(const int16_t *pcm, uint32_t frames) {
    if (!g_ready || !pcm || !frames) return 0;
    ac97_tick();
    uint32_t fed = 0;
    while (fed < frames) {
        uint32_t space = AU_BUFSAMP - g_fillpos;   /* samples left in cur desc */
        if (space == 0) {
            /* about to commit a descriptor: only advance while the queue
             * stays <= N-3 ahead of CIV (keeps 3 descs of runway) */
            uint8_t civ = (uint8_t)bd_civ();
            uint32_t queued = (g_cur + AU_NDESC - (uint32_t)civ) % AU_NDESC;
            if (queued >= AU_NDESC - 3) break;
            g_cur = (g_cur + 1) % AU_NDESC;
            g_fillpos = 0;
            /* LVI = last fully-written descriptor; writing it while the
             * channel is halted also auto-resumes the DMA. */
            uint8_t lastvalid = (uint8_t)((g_cur + AU_NDESC - 1) % AU_NDESC);
            outb(g_nabm + NABM_LVI, lastvalid);
            continue;
        }
        uint32_t room_frames = space / 2;          /* stereo frames */
        uint32_t todo = frames - fed;
        if (todo > room_frames) todo = room_frames;
        uint16_t *dst = (uint16_t *)(g_pool + (uint64_t)g_cur * AU_BUFSAMP * 2)
                        + g_fillpos;
        const int16_t *src = pcm + fed * 2;
        for (uint32_t i = 0; i < todo * 2; ++i) dst[i] = (uint16_t)src[i];
        g_fillpos += todo * 2;
        fed += todo;
    }
    return fed;
}

void ac97_flush(void) {
    if (!g_ready) return;
    /* CR.RR resets the channel: BDBAR/LVI/CIV/PICB all zeroed - BDBAR MUST
     * be rewritten or the DMA fetches descriptors from physical 0. */
    outb(g_nabm + NABM_CR, PO_CR_RR);
    for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause");
    outl(g_nabm + NABM_BDBAR, (uint32_t)g_bd_phys);
    memset(g_pool, 0, POOL_BYTES);
    g_cur = 0; g_fillpos = 0;
    outb(g_nabm + NABM_LVI, 0);
    outb(g_nabm + NABM_CR, PO_CR_RPBM);   /* resume on zeroed (silent) ring */
}

/* approx ms of audio still buffered in the DMA ring (for pacing + LCD) */
uint32_t ac97_level(void) {
    ac97_tick();
    if (!g_ready) return 0;
    uint8_t civ = (uint8_t)bd_civ();
    /* descriptors civ..g_cur-1 are complete; g_cur holds g_fillpos samples */
    uint32_t full = (g_cur + AU_NDESC - (uint32_t)civ) % AU_NDESC;
    uint64_t frames = (uint64_t)full * (AU_BUFSAMP / 2) + g_fillpos / 2;
    return (uint32_t)(frames * 1000 / (g_rate ? g_rate : 48000));
}

/* ---------- real-time spectrum (for visualisers) ----------
 * Reads the stereo window the DMA is playing RIGHT NOW (CIV + PICB) and
 * runs a Goertzel filterbank - so bars dance to the actual audio, not a
 * fake animation. Window stays inside the current BD which is fully
 * written by definition, so no stale/unwritten reads. */

#define AC97_SPEC_N 1024            /* analysis window: ~21ms @48k */

/* 2*cos(2*pi*f/48000) in Q15, log-spaced 40Hz..16kHz */
static const int32_t g_goertzel[AC97_SPEC_BANDS] = {
     65535, // 40 Hz
     65534, // 55 Hz
     65533, // 75 Hz
     65530, // 103 Hz
     65525, // 141 Hz
     65515, // 194 Hz
     65496, // 265 Hz
     65462, // 364 Hz
     65397, // 498 Hz
     65274, // 683 Hz
     65044, // 937 Hz
     64613, // 1284 Hz
     63805, // 1760 Hz
     62296, // 2412 Hz
     59493, // 3306 Hz
     54337, // 4532 Hz
     45034, // 6213 Hz
     28865, // 8516 Hz
      2807, // 11673 Hz
    -32768, // 16000 Hz
};

void ac97_spectrum(uint8_t out[AC97_SPEC_BANDS]) {
    /* ~20k int64 MACs per call - at 60fps that was burning real CPU on a
     * slow VM (user: "cpu speed makes song feel slower"). Recompute at
     * ~15Hz and serve the cached frame otherwise; bars still track beats. */
    static uint8_t cached[AC97_SPEC_BANDS];
    static int tick;
    if ((++tick & 3) != 0) { memcpy(out, cached, AC97_SPEC_BANDS); return; }
    memset(cached, 0, AC97_SPEC_BANDS);
    if (!g_ready) { memcpy(out, cached, AC97_SPEC_BANDS); return; }
    ac97_tick();
    uint8_t civ = (uint8_t)bd_civ();
    uint16_t picb = inw(g_nabm + 0x18);   /* 16-bit samples played in BD */
    uint32_t f0 = (uint32_t)(picb >> 1);  /* -> stereo frame index */
    if (f0 + AC97_SPEC_N > AU_BUFSAMP / 2)
        f0 = AU_BUFSAMP / 2 - AC97_SPEC_N;   /* slide back, stay in-BD */
    const uint8_t *base = g_pool + (uint64_t)civ * AU_BUFSAMP * 2 +
                          (uint64_t)f0 * 4;
    for (int b = 0; b < AC97_SPEC_BANDS; b++) {
        int64_t s1 = 0, s2 = 0;
        int32_t coef = g_goertzel[b];
        for (int n = 0; n < AC97_SPEC_N; n++) {
            const int16_t *s = (const int16_t *)(base + (uint64_t)n * 4);
            int32_t x = ((int32_t)s[0] + (int32_t)s[1]) >> 1;   /* mono */
            int64_t s0 = x + ((int64_t)coef * s1 >> 15) - s2;
            s2 = s1; s1 = s0;
        }
        /* |s| ~ N*A/2 for a sine of amplitude A; m ~ N*A -> /N/100 -> 0..255 */
        int64_t m = (s1 < 0 ? -s1 : s1) + (s2 < 0 ? -s2 : s2);
        uint32_t v = (uint32_t)(m / ((int64_t)AC97_SPEC_N * 100));
        if (v > 255) v = 255;
        cached[b] = (uint8_t)v;
    }
    memcpy(out, cached, AC97_SPEC_BANDS);
}

/* PCM integrity diagnostic: stats + raw samples of what the DAC is
 * playing RIGHT NOW. Tells us if the stream reaching hardware is sane
 * music (varying L/R, no DC, no clipping) or corrupted. */
void ac97_diag(void) {
    if (!g_ready) { kprintf("ac97: not ready\n"); return; }
    ac97_tick();
    uint8_t civ = (uint8_t)bd_civ();
    uint16_t picb = inw(g_nabm + 0x18);
    uint32_t f0 = (uint32_t)(picb >> 1);
    if (f0 + 512 > AU_BUFSAMP / 2) f0 = AU_BUFSAMP / 2 - 512;
    const int16_t *s = (const int16_t *)(g_pool + (uint64_t)civ * AU_BUFSAMP * 2 +
                                         (uint64_t)f0 * 4);
    int64_t sum_l = 0, sum_r = 0, sum_ll = 0, sum_rr = 0, sum_lr = 0;
    int peak_l = 0, peak_r = 0, zcr = 0, same_lr = 0;
    int16_t prev_l = 0;
    for (int i = 0; i < 512; i++) {
        int l = s[i * 2], r = s[i * 2 + 1];
        if (l < 0 ? -l > peak_l : l > peak_l) peak_l = l < 0 ? -l : l;
        if (r < 0 ? -r > peak_r : r > peak_r) peak_r = r < 0 ? -r : r;
        sum_l += l; sum_r += r;
        sum_ll += (int64_t)l * l; sum_rr += (int64_t)r * r;
        sum_lr += (int64_t)l * r;
        if (l == r) same_lr++;
        if (i && ((l ^ prev_l) < 0)) zcr++;
        prev_l = l;
    }
    /* corr^2 = lr^2/(ll*rr) in percent - tells us if L/R are independent
     * (real stereo music ~30-90%) or identical (mono ~100%) or noise */
    uint32_t corr2 = (sum_ll && sum_rr) ?
        (uint32_t)((sum_lr * sum_lr * 10000) / (sum_ll * sum_rr)) : 0;
    /* kprintf has no %lld - cast to int (values are small) */
    kprintf("ac97: DIAG civ=%d picb=%u pkL=%d pkR=%d dcL=%d dcR=%d zcr=%d sameLR=%d/512 corr2=%u%%\n",
            civ, picb, peak_l, peak_r,
            (int)(sum_l / 512), (int)(sum_r / 512),
            zcr, same_lr, corr2);
    /* first 16 stereo frames hex for eyeballing */
    for (int i = 0; i < 16; i++)
        kprintf("  [%2d] L=%6d R=%6d\n", i, s[i * 2], s[i * 2 + 1]);
}

/* serial diagnostic: proves whether the DAC is actually consuming audio.
 * PICB = played sample count - if it never advances, the DMA/emulator
 * path is dead even though buffers are queued. */
void ac97_debug(void) {
    if (!g_ready) { kprintf("ac97: not ready\n"); return; }
    uint16_t picb = inw(g_nabm + 0x18);
    uint16_t sr   = inw(g_nabm + NABM_SR);
    uint32_t gsta = inl(g_nabm + NABM_GLB_S);
    uint16_t mv   = inw(g_nam + 0x02), pv = inw(g_nam + 0x18);
    kprintf("ac97: civ=%d cur=%u fill=%u lvi=%d picb=%u sr=%u gsta=%x mv=%x pv=%x lvl=%u\n",
            bd_civ(), g_cur, g_fillpos, inb(g_nabm + NABM_LVI), picb, sr, gsta,
            mv, pv, ac97_level());
}
