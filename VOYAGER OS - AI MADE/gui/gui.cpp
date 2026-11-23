/* Window manager + desktop - Squarebin look.
 * Pure C-style function pointers so apps can be written in C or C++.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../kernel/k.h"
#include "../kernel/fb.h"
#include "../kernel/settings.h"
#include "../kernel/vfs.h"
#include "../kernel/sysvol.h"
#include "../kernel/arch/arch.h"
#include "gui.h"
#include "assets_gen.h"
#include "../net/net.h"
#include "../drivers/e1000.h"
#include "../drivers/ata.h"

#define MAX_WINDOWS 12
#define TITLE_H    28
#define MENU_H     24
#define DOCK_ICON  48
#define DOCK_PAD   16
#define DOCK_H     64
#define DESK_ICON_SZ 72

/* ---------- colors ---------- */
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
/* lighten (+) or darken (-) an XRGB color by delta, clamped */
uint32_t gui_shade(uint32_t c, int delta) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += delta; g += delta; b += delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* ---------- assets ---------- */

#define MAX_IMAGES 160
struct CachedImage {
    char name[32];
    const uint8_t *rgba;
    uint32_t w, h;
    bool used;
};
static CachedImage g_images[MAX_IMAGES];

const uint8_t *asset_png(const char *name, size_t *size) {
    for (int i = 0; i < assets_count; i++) {
        if (!strcasecmp(assets[i].name, name)) {
            if (size) *size = assets[i].size;
            return assets[i].data;
        }
    }
    return NULL;
}

static const uint8_t *image_data(const char *name, uint32_t *w, uint32_t *h) {
    /* cache lookup */
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_images[i].used && !strcasecmp(g_images[i].name, name)) {
            *w = g_images[i].w; *h = g_images[i].h;
            return g_images[i].rgba;
        }
    }
    /* Decode + cache with IRQs masked. png_decode is CPU-bound; an interrupt
     * in the decode/return/cache window corrupts the result, so keep the
     * whole critical section atomic. */
    __asm__ volatile("cli");
    const uint8_t *ret = NULL;
    size_t sz;
    const uint8_t *png = asset_png(name, &sz);
    uint8_t *rgba = NULL;
    uint32_t iw = 0, ih = 0;
    if (png) {
        int prc = png_decode(png, sz, &rgba, &iw, &ih);
        if (prc) { if (rgba) kfree(rgba); rgba = NULL; }
    }
    __asm__ volatile("sti");
    if (!rgba) {
        /* user-supplied icon: "<name>.png" in the VFS root doubles as the
         * app icon (e.g. "Voyage Player.png" = the MICON artwork). Try the
         * named file first, then the raw MICON names in case the desktop
         * file was never renamed. gui_vfs_image (not _thumb) so a transient
         * miss isn't cached as a permanent failure. */
        char vn[72];
        snprintf(vn, sizeof(vn), "%s.png", name);
        const char *cands[3] = { vn, "MICON.png", "micon.png" };
        for (int ci = 0; ci < 3 && !rgba; ci++) {
            const uint8_t *im = gui_vfs_image("", cands[ci], &iw, &ih);
            if (!im) continue;
            rgba = (uint8_t *)kmalloc((size_t)iw * ih * 4);
            if (rgba) memcpy(rgba, im, (size_t)iw * ih * 4);
            else iw = ih = 0;
        }
    }
    if (rgba) {
        __asm__ volatile("cli");
        int slot = -1;
        for (int i = 0; i < MAX_IMAGES; i++)
            if (!g_images[i].used) { slot = i; break; }
        if (slot >= 0) {
            g_images[slot].used = true;
            strncpy(g_images[slot].name, name, 31);
            g_images[slot].rgba = rgba;
            g_images[slot].w = iw; g_images[slot].h = ih;
            *w = iw; *h = ih;
            ret = rgba;
        } else {
            kfree(rgba);
        }
        __asm__ volatile("sti");
    }
    return ret;
}

const uint8_t *asset_image(const char *name, uint32_t *w, uint32_t *h) {
    return image_data(name, w, h);
}

/* ---- VFS image previews (PNG files stored in the user filesystem) ----
 * vfs_read_raw_in returns the raw 512-byte sector so binary PNGs survive;
 * decodes are cached per "dir/name" so icon redraws don't re-decode. */
#define VIMG_CACHE 2   /* full-res decode cache: viewer only (icons use
                         * the small thumbnail caches below) */
#define MAX_SECTORS_MAX 60
#define SIMG_CACHE 2
typedef struct {
    char     key[128];
    uint8_t *rgba;
    uint32_t w, h;
    uint64_t tick;
    int      used;
    int      failed;   /* undecodable: don't retry every frame */
} SImgSlot;
static SImgSlot g_simg[SIMG_CACHE];

/* decoded RGBA for an ISO system-volume PNG (path like "boot/assets/x.png").
 * Read-only: system images can be viewed but never modified. */
const uint8_t *gui_sysvol_image(const char *path, uint32_t *w, uint32_t *h) {
    if (w) *w = 0; if (h) *h = 0;
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < SIMG_CACHE; i++)
        if (g_simg[i].used && !strcmp(g_simg[i].key, path)) {
            if (g_simg[i].failed) return NULL;
            *w = g_simg[i].w; *h = g_simg[i].h;
            g_simg[i].tick = pit_ticks();
            return g_simg[i].rgba;
        }
    const SysVolEnt *se = sysvol_lookup(path);
    const uint8_t *blob; uint32_t blen = 0;
    uint8_t *rgba = NULL;
    uint32_t iw = 0, ih = 0;
    int ok = se && se->has_blob &&
             sysvol_blob(se, &blob, &blen) == 0;
    /* mask IRQs during decode like gui_vfs_image: the decoder is CPU-bound
     * and a timer/keyboard interrupt mid-run corrupts its inflate state */
    __asm__ volatile("cli");
    int drc = ok ? png_decode(blob, blen, &rgba, &iw, &ih) : -1;
    __asm__ volatile("sti");
    ok = ok && drc == 0 && rgba;
    int slot = 0;
    for (int i = 0; i < SIMG_CACHE; i++)
        if (!g_simg[i].used) { slot = i; break; }
        else if (g_simg[i].tick < g_simg[slot].tick) slot = i;
    if (g_simg[slot].used && g_simg[slot].rgba) kfree(g_simg[slot].rgba);
    strncpy(g_simg[slot].key, path, sizeof(g_simg[slot].key) - 1);
    g_simg[slot].key[sizeof(g_simg[slot].key) - 1] = 0;
    g_simg[slot].rgba = ok ? rgba : NULL;
    g_simg[slot].w = iw; g_simg[slot].h = ih;
    g_simg[slot].tick = pit_ticks();
    g_simg[slot].used = 1;
    g_simg[slot].failed = !ok;
    if (!ok) return NULL;
    *w = iw; *h = ih;
    return rgba;
}

typedef struct {
    char     key[80];
    uint8_t *rgba;
    uint32_t w, h;
    uint64_t tick;
    int      used;
} VImgSlot;
static VImgSlot g_vimg[VIMG_CACHE];

const uint8_t *gui_vfs_image(const char *dir, const char *name,
                             uint32_t *w, uint32_t *h) {
    if (w) *w = 0; if (h) *h = 0;
    if (!name || !name[0]) return NULL;
    char key[80];
    snprintf(key, sizeof(key), "%s%s%s", dir ? dir : "",
             (dir && dir[0]) ? "/" : "", name);
    for (int i = 0; i < VIMG_CACHE; i++)
        if (g_vimg[i].used && !strcmp(g_vimg[i].key, key)) {
            *w = g_vimg[i].w; *h = g_vimg[i].h;
            g_vimg[i].tick = pit_ticks();
            return g_vimg[i].rgba;
        }
    int fsz = vfs_file_size_in(dir, name);
    if (fsz <= 0 || fsz > MAX_SECTORS_MAX * 512) return NULL;
    uint8_t *raw = (uint8_t *)kzalloc((size_t)fsz + 512);
    if (!raw) return NULL;
    int got = vfs_read_raw_full_in(dir, name, raw, fsz + 512);
    __asm__ volatile("cli");
    uint8_t *rgba = NULL;
    uint32_t iw = 0, ih = 0;
    int rc = png_decode(raw, got > 0 ? got : fsz, &rgba, &iw, &ih);
    __asm__ volatile("sti");
    kfree(raw);
    if (rc || !rgba) { if (rgba) kfree(rgba); return NULL; }
    /* evict the oldest slot */
    int slot = 0;
    for (int i = 0; i < VIMG_CACHE; i++)
        if (!g_vimg[i].used) { slot = i; break; }
        else if (g_vimg[i].tick < g_vimg[slot].tick) slot = i;
    if (g_vimg[slot].used && g_vimg[slot].rgba) kfree(g_vimg[slot].rgba);
    strncpy(g_vimg[slot].key, key, sizeof(g_vimg[slot].key) - 1);
    g_vimg[slot].key[sizeof(g_vimg[slot].key) - 1] = 0;
    g_vimg[slot].rgba = rgba;
    g_vimg[slot].w = iw; g_vimg[slot].h = ih;
    g_vimg[slot].tick = pit_ticks();
    g_vimg[slot].used = 1;
    *w = iw; *h = ih;
    return rgba;
}

/* ---- icon thumbnails ----
 * Icons only ever need ~96px, but the full decodes above cache multi-MB
 * bitmaps (1280x960 wallpapers ~5MB each) - a few of those fill the heap
 * and every scroll re-decodes -> the "OS lags browsing files" bug. These
 * caches store a small downscaled copy per image, so scrolling through a
 * folder of PNGs decodes each image ONCE, ever. */
#define TUMB_CACHE 64
#define TUMB_MAX   96
typedef struct {
    char     key[160];
    uint8_t *rgba;
    uint32_t w, h;
    uint64_t tick;
    int      used, failed;
} TSlot;
static TSlot g_vtumb[TUMB_CACHE];
static TSlot g_stumb[TUMB_CACHE];

/* nearest-neighbour downscale to <= TUMB_MAX on the long side */
static const uint8_t *make_thumb(const uint8_t *src, uint32_t sw,
                                 uint32_t sh, uint32_t *ow, uint32_t *oh) {
    uint32_t m = sw > sh ? sw : sh;
    if (m <= TUMB_MAX) {
        /* already small: still copy so the cache owns the pixels */
        uint8_t *c = (uint8_t *)kmalloc((size_t)sw * sh * 4);
        if (!c) return NULL;
        memcpy(c, src, (size_t)sw * sh * 4);
        *ow = sw; *oh = sh;
        return c;
    }
    uint32_t nw = sw * TUMB_MAX / m, nh = sh * TUMB_MAX / m;
    if (!nw) nw = 1; if (!nh) nh = 1;
    uint8_t *d = (uint8_t *)kmalloc((size_t)nw * nh * 4);
    if (!d) return NULL;
    for (uint32_t y = 0; y < nh; y++) {
        uint32_t sy = y * sh / nh;
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t sx = x * sw / nw;
            memcpy(d + ((size_t)y * nw + x) * 4,
                   src + ((size_t)sy * sw + sx) * 4, 4);
        }
    }
    *ow = nw; *oh = nh;
    return d;
}

static const uint8_t *tumb_lookup(TSlot *cache, const char *key,
                                  uint32_t *w, uint32_t *h) {
    for (int i = 0; i < TUMB_CACHE; i++)
        if (cache[i].used && !strcmp(cache[i].key, key)) {
            if (cache[i].failed) return NULL;
            cache[i].tick = pit_ticks();
            *w = cache[i].w; *h = cache[i].h;
            return cache[i].rgba;
        }
    return NULL;
}
static void tumb_store(TSlot *cache, const char *key, const uint8_t *rgba,
                       uint32_t w, uint32_t h, int failed) {
    int slot = 0;
    for (int i = 0; i < TUMB_CACHE; i++)
        if (!cache[i].used) { slot = i; break; }
        else if (cache[i].tick < cache[slot].tick) slot = i;
    if (cache[slot].used && cache[slot].rgba) kfree(cache[slot].rgba);
    strncpy(cache[slot].key, key, sizeof(cache[slot].key) - 1);
    cache[slot].key[sizeof(cache[slot].key) - 1] = 0;
    cache[slot].rgba = failed ? NULL : (uint8_t *)rgba;
    cache[slot].w = w; cache[slot].h = h;
    cache[slot].tick = pit_ticks();
    cache[slot].used = 1;
    cache[slot].failed = failed;
}

/* thumbnail for a user VFS .png (dir "" = desktop root) */
const uint8_t *gui_vfs_thumb(const char *dir, const char *name,
                             uint32_t *w, uint32_t *h) {
    if (w) *w = 0; if (h) *h = 0;
    if (!name || !name[0]) return NULL;
    char key[160];
    snprintf(key, sizeof(key), "V:%s%s%s", dir ? dir : "",
             (dir && dir[0]) ? "/" : "", name);
    const uint8_t *hit = tumb_lookup(g_vtumb, key, w, h);
    if (hit) return hit;
    uint32_t fw = 0, fh = 0;
    const uint8_t *full = gui_vfs_image(dir, name, &fw, &fh);
    if (!full) { tumb_store(g_vtumb, key, NULL, 0, 0, 1); return NULL; }
    uint32_t tw = 0, th = 0;
    const uint8_t *t = make_thumb(full, fw, fh, &tw, &th);
    if (!t) return NULL;
    tumb_store(g_vtumb, key, t, tw, th, 0);
    *w = tw; *h = th;
    return t;
}

/* thumbnail for an ISO system-volume .png (path like "boot/assets/x.png") */
const uint8_t *gui_sysvol_thumb(const char *path, uint32_t *w, uint32_t *h) {
    if (w) *w = 0; if (h) *h = 0;
    if (!path || !path[0]) return NULL;
    char key[160];
    snprintf(key, sizeof(key), "S:%s", path);
    const uint8_t *hit = tumb_lookup(g_stumb, key, w, h);
    if (hit) return hit;
    uint32_t fw = 0, fh = 0;
    const uint8_t *full = gui_sysvol_image(path, &fw, &fh);
    if (!full) { tumb_store(g_stumb, key, NULL, 0, 0, 1); return NULL; }
    uint32_t tw = 0, th = 0;
    const uint8_t *t = make_thumb(full, fw, fh, &tw, &th);
    if (!t) return NULL;
    tumb_store(g_stumb, key, t, tw, th, 0);
    *w = tw; *h = th;
    return t;
}

int asset_index(const char *name) {
    return asset_png(name, NULL) ? 1 : -1;
}

struct Window;
static Window *g_cur_win;   /* window currently receiving draw/event */
static int g_cur_cx, g_cur_cy; /* content origin (global) */
static int32_t g_mx = 400, g_my = 300; /* last mouse pos (for hover FX) */
static uint8_t g_mbuttons;           /* current mouse button bitmask */

void asset_draw(int x, int y, int size, const char *name) {
    uint32_t w, h;
    const uint8_t *img = image_data(name, &w, &h);
    if (!img) return;
    /* inside a window, offset into content space like app_fill/app_text;
     * outside (desktop, dock, titlebar) coords are already global */
    if (g_cur_win) { x += g_cur_cx; y += g_cur_cy; }
    fb_draw_image_scaled(x, y, size, size, img, w, h);
}

void rtc_get_time(char *buf, size_t size);
void rtc_date_parts(int *yy, int *mm, int *dd, int *wd);
extern "C" {
int  ac97_volume(void);
void ac97_set_volume(int pct);
int  gui_is_audio(const char *name);
}

/* ---------- app registry ---------- */

static App *g_apps[32];
static int g_app_count;

/* ---------- dock entries: pinned core apps + user-added apps/files ----------
 * The dock only shows the core apps by default; everything else is added via
 * right-click "Add to Dock" or by dropping an item on the dock. The bar
 * widens to fit more icons (same size); when it would overflow the screen
 * it scrolls with < > buttons. */
#define DOCK_MAX 24
struct DockEnt {
    int kind;                     /* 0 = app, 1 = VFS file/folder */
    int app;                      /* g_apps index (kind 0) */
    int pinned;                   /* 1 = core app: permanent, can't remove */
    char name[VFS_NAME_LEN + 1];  /* vfs name (kind 1) */
    char dir[VFS_NAME_LEN + 1];   /* vfs parent folder, "" = root */
};
static struct DockEnt g_dock[DOCK_MAX];
static int g_dock_n;
static int g_dock_scroll;         /* first visible entry index */
static int g_dock_vis;            /* entries that fit this frame */
static int g_dock_cx[DOCK_MAX];   /* screen-x of each VISIBLE icon center */
static int g_dock_iy[DOCK_MAX];   /* screen-y of each VISIBLE icon top */
static uint64_t g_dock_launch[DOCK_MAX]; /* launch anim per visible slot */
static int g_dock_press = -1;     /* dock entry index held, awaiting release */
static int g_dock_press_x, g_dock_press_y;
static bool g_dock_out;           /* dragged out of the dock as shortcut */
static int g_dock_menu = -1;      /* dock entry with context menu open */
static int g_dock_menu_x, g_dock_menu_y;
static int g_dock_menu_app;       /* 1 = app entry (Close All + Remove) */
static int g_dock_menu_w, g_dock_menu_h;

static const char *kDockPinned[] = {
    "Files", "Voyage Web", "Quill", "Calc", "Console", "Settings", "Memo", "Voyage Player"
};
static int dock_is_pinned_app(const char *name) {
    for (int i = 0; i < (int)(sizeof(kDockPinned) / sizeof(kDockPinned[0])); i++)
        if (!strcasecmp(kDockPinned[i], name)) return 1;
    return 0;
}
static int dock_find_app(int app_idx) {
    for (int i = 0; i < g_dock_n; i++)
        if (g_dock[i].kind == 0 && g_dock[i].app == app_idx) return i;
    return -1;
}
static int dock_find_vfs(const char *dir, const char *name) {
    for (int i = 0; i < g_dock_n; i++)
        if (g_dock[i].kind == 1 && !strcmp(g_dock[i].name, name) &&
            !strcmp(g_dock[i].dir, dir ? dir : "")) return i;
    return -1;
}
static void dock_persist(void) {
    /* serialize only user-added (non-pinned) entries */
    char buf[256]; int n = 0;
    buf[0] = 0;
    for (int i = 0; i < g_dock_n; i++) {
        if (g_dock[i].kind == 0) {
            if (g_dock[i].pinned) continue;
            n += snprintf(buf + n, sizeof(buf) - n, "A|%s\n",
                          g_apps[g_dock[i].app]->name);
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "F|%s|%s\n",
                          g_dock[i].dir, g_dock[i].name);
        }
        if (n >= (int)sizeof(buf) - 40) break;
    }
    settings_save_dock(buf);
}
/* (re)build the dock: pinned apps first, then saved user pins */
static void dock_build(void) {
    g_dock_n = 0;
    for (int i = 0; i < g_app_count && g_dock_n < DOCK_MAX; i++) {
        if (!dock_is_pinned_app(g_apps[i]->name)) continue;
        struct DockEnt *e = &g_dock[g_dock_n++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->app = i; e->pinned = 1;
    }
    const char *d = settings_dock();
    if (d) {
        char tmp[256];
        strncpy(tmp, d, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
        for (char *ln = strtok(tmp, "\n"); ln && g_dock_n < DOCK_MAX;
             ln = strtok(NULL, "\n")) {
            if (ln[0] == 'A' && ln[1] == '|' && g_dock_n < DOCK_MAX) {
                App *a = gui_find_app(ln + 2);
                int ai = -1;
                for (int k = 0; k < g_app_count; k++)
                    if (g_apps[k] == a) { ai = k; break; }
                if (ai >= 0 && !dock_is_pinned_app(a->name) &&
                    dock_find_app(ai) < 0) {
                    struct DockEnt *e = &g_dock[g_dock_n++];
                    memset(e, 0, sizeof(*e));
                    e->kind = 0; e->app = ai;
                }
            } else if (ln[0] == 'F' && ln[1] == '|') {
                char *p2 = strchr(ln + 2, '|');
                if (!p2) continue;
                *p2 = 0;
                const char *dir = ln + 2, *nm = p2 + 1;
                if (dock_find_vfs(dir, nm) < 0 && nm[0]) {
                    struct DockEnt *e = &g_dock[g_dock_n++];
                    memset(e, 0, sizeof(*e));
                    e->kind = 1;
                    strncpy(e->dir, dir, sizeof(e->dir) - 1);
                    strncpy(e->name, nm, sizeof(e->name) - 1);
                }
            }
        }
    }
    g_dock_scroll = 0;
}
int gui_dock_add_app(const char *appname) {
    App *a = gui_find_app(appname);
    if (!a) return -1;
    int ai = -1;
    for (int k = 0; k < g_app_count; k++)
        if (g_apps[k] == a) { ai = k; break; }
    if (ai < 0) return -1;
    if (dock_find_app(ai) >= 0) return 0;      /* already there */
    if (g_dock_n >= DOCK_MAX) return -1;
    struct DockEnt *e = &g_dock[g_dock_n++];
    memset(e, 0, sizeof(*e));
    e->kind = 0; e->app = ai;
    dock_persist();
    return 0;
}
int gui_dock_add_vfs(const char *dir, const char *name) {
    if (dock_find_vfs(dir, name) >= 0) return 0;
    if (g_dock_n >= DOCK_MAX) return -1;
    struct DockEnt *e = &g_dock[g_dock_n++];
    memset(e, 0, sizeof(*e));
    e->kind = 1;
    strncpy(e->dir, dir ? dir : "", sizeof(e->dir) - 1);
    strncpy(e->name, name, sizeof(e->name) - 1);
    dock_persist();
    return 0;
}
static void dock_remove(int idx) {
    if (idx < 0 || idx >= g_dock_n || g_dock[idx].pinned) return;
    memmove(g_dock + idx, g_dock + idx + 1,
            (g_dock_n - idx - 1) * sizeof(g_dock[0]));
    g_dock_n--;
    if (g_dock_scroll > g_dock_n - 1) g_dock_scroll = g_dock_n - 1;
    if (g_dock_scroll < 0) g_dock_scroll = 0;
    dock_persist();
}

void gui_register_app(App *app) {
    if (g_app_count < 32) g_apps[g_app_count++] = app;
}

int gui_app_count(void) { return g_app_count; }
App *gui_get_app(int i) { return (i >= 0 && i < g_app_count) ? g_apps[i] : NULL; }
App *gui_find_app(const char *name) {
    for (int i = 0; i < g_app_count; i++)
        if (!strcasecmp(g_apps[i]->name, name)) return g_apps[i];
    return NULL;
}

/* ---------- windows ---------- */

struct Window {
    int id;
    App *app;
    void *ctx;
    int x, y, w, h;
    bool visible;
    bool focused;
    bool dragging;
    int drag_ox, drag_oy;
    bool minimized;
    int z;          /* stacking order: higher = closer to front */
    int closing;        /* spin-out close animation in progress */
    uint64_t close_t;   /* when the close was requested */
    int cx0, cy0, cw0, ch0;   /* geometry at close start (anim origin) */
    uint64_t open_t;        /* window-open scale-in animation start (0=none);
                               reuses snap/snap_bands for the source image */
    uint32_t *snap;     /* pixel snapshot of the whole window (frame +
                           content) so EVERYTHING shrinks during spin-out */
    int snap_w, snap_h; /* snapshot pixel dims (may be clipped) */
    int snap_ox, snap_oy;/* snapshot origin offset inside the window */
    /* fallback when one big snapshot kmalloc fails (fragmented heap):
     * the window is rendered into horizontal bands instead */
    uint32_t **snap_bands;
    int snap_nb, snap_band_rows;
    int parent_id;      /* id of the window that spawned this app (-1 none);
                           closing returns focus there instead of whatever
                           same-app instance happens to be on top */
    bool maximized;     /* zoomed to fill the screen; dock auto-hides */
    int min_w, min_h;   /* shrink floor = the app's designed size */
    bool no_resize;     /* fixed-size app: no grip, no zoom */
    int min_out;            /* minimize spin-out playing (window still
                               visible until the anim completes) */
    uint64_t min_t;         /* minimize spin-out start time */
    int zooming;            /* zoom boing animation in progress */
    uint64_t zoom_t;        /* zoom animation start time */
    int zfx, zfy, zfw, zfh; /* zoom: rect at anim start */
    int ztx, zty, ztw, zth; /* zoom: rect at anim end   */
    int rx, ry, rw, rh;     /* saved normal rect: zoom restores
                               the SAME frame the window had,
                               not a generic centered size */
};
static Window g_win[MAX_WINDOWS];
static int g_win_count;
static int g_next_win_id = 1;
static int g_focus = -1;
static int g_z_top;

static void close_window_idx(int idx);
static void minimize_window_idx(int idx);
static void restore_window_spin(int idx);
static void capture_window_snapshot(Window *w, int idx);
static void free_window_snapshot(Window *w);

static int content_x(Window *w) { return w->x; }
static int content_y(Window *w) { return w->y + TITLE_H; }
static int content_w(Window *w) { return w->w; }
static int content_h(Window *w) { return w->h - TITLE_H; }

bool gui_app_is_open(App *app) {
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].app == app && g_win[i].visible && !g_win[i].minimized)
            return true;
    return false;
}

/* taskbar helpers: count EVERY window of an app (incl. minimized),
 * find its focused window and its topmost (highest-z) window */
static int app_window_count(App *a) {
    int n = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].app == a) n++;
    return n;
}
static int app_focused_idx(App *a) {
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].app == a && g_win[i].focused &&
            !g_win[i].minimized && g_win[i].visible)
            return i;
    return -1;
}
/* true when the app has windows but EVERY one is minimized */
static bool app_all_minimized(App *a) {
    bool any = false;
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].app == a) {
            any = true;
            if (!g_win[i].minimized) return false;
        }
    return any;
}
static int app_top_idx(App *a) {
    int top = -1;
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].app == a && (top < 0 || g_win[i].z > g_win[top].z)) top = i;
    return top;
}

static void focus_window(int idx) {
    if (idx < 0 || idx >= g_win_count) return;
    for (int i = 0; i < g_win_count; i++) g_win[i].focused = false;
    g_win[idx].focused = true;
    g_win[idx].z = ++g_z_top;   /* raise to front */
    g_focus = idx;
}

/* focus WITHOUT raising: used when a window closes so the remaining
 * windows keep their stacking order ("stay as is") instead of the next
 * app popping to the front over a fullscreen/maximized one */
static void focus_window_inplace(int idx) {
    if (idx < 0 || idx >= g_win_count) return;
    for (int i = 0; i < g_win_count; i++) g_win[i].focused = false;
    g_win[idx].focused = true;
    g_focus = idx;
}

void gui_focus_window(int win_id) {
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].id == win_id) { focus_window(i); return; }
}

void gui_close_window(int win_id) {
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].id == win_id) { close_window_idx(i); return; }
}
/* lets an app close the window that is currently handling its event
 * (e.g. Quill's save-picker finishing a save) */
void gui_close_current_window(void) {
    if (!g_cur_win) return;
    for (int i = 0; i < g_win_count; i++)
        if (&g_win[i] == g_cur_win) { close_window_idx(i); return; }
}

void gui_open_app(App *app) {
    if (!app || !app->create) return;
    /* if already open, focus it */
    for (int i = g_win_count - 1; i >= 0; i--) {
        if (g_win[i].app == app && !g_win[i].minimized) {
            focus_window(i);
            return;
        }
    }
    if (g_win_count >= MAX_WINDOWS) return;
    Window *w = &g_win[g_win_count];
    memset(w, 0, sizeof(*w));
    w->id = g_next_win_id++;
    w->app = app;
    w->ctx = app->create ? app->create() : NULL;
    w->w = app->width ? app->width : 640;
    w->h = app->height ? app->height : 480;
    w->x = 80 + (g_win_count * 30) % 200;
    w->y = 60 + (g_win_count * 24) % 160;
    w->visible = true;
    w->minimized = false;
    /* shrink floor = designed size (you can enlarge up to the screen,
     * just not crush the layout); fixed-size apps (Calculator) get no
     * grip and no zoom */
    w->min_w = app->width  ? app->width  : 240;
    w->min_h = app->height ? app->height : 170;
    if (w->min_w > (int)fb_width())  w->min_w = (int)fb_width();
    if (w->min_h > (int)fb_height() - MENU_H - DOCK_H)
        w->min_h = (int)fb_height() - MENU_H - DOCK_H;
    w->no_resize = app->no_resize != 0;
    /* remember who spawned us (app event handlers run with g_cur_win set):
     * closing this window returns focus to that window, not to whatever
     * unrelated instance of the same app sits on top */
    w->parent_id = g_cur_win ? g_cur_win->id : 0;
    /* register the window BEFORE init: app->init may block (e.g. Voyage Web's
     * synchronous page fetch) and the net-yield path keeps repainting, so
     * the window must already be counted to be drawn during that wait. */
    int newidx = g_win_count;
    g_win_count++;
    focus_window(newidx);
    if (app->init) app->init(w->ctx);
    /* scale-in open animation: snapshot the first frame, then grow it
     * from ~55% to full size over ~280ms (see draw_window) */
    capture_window_snapshot(w, newidx);
    w->open_t = pit_ticks();
}

/* open a NEW window instance even if the app is already running
 * (multitasking: several windows of the same app at once) */
void gui_open_app_new(App *app) {
    if (!app || !app->create || g_win_count >= MAX_WINDOWS) return;
    Window *w = &g_win[g_win_count];
    memset(w, 0, sizeof(*w));
    w->id = g_next_win_id++;
    w->app = app;
    w->ctx = app->create ? app->create() : NULL;
    w->w = app->width ? app->width : 640;
    w->h = app->height ? app->height : 480;
    w->x = 80 + (g_win_count * 30) % 200;
    w->y = 60 + (g_win_count * 24) % 160;
    w->visible = true;
    w->minimized = false;
    w->min_w = app->width  ? app->width  : 240;
    w->min_h = app->height ? app->height : 170;
    if (w->min_w > (int)fb_width())  w->min_w = (int)fb_width();
    if (w->min_h > (int)fb_height() - MENU_H - DOCK_H)
        w->min_h = (int)fb_height() - MENU_H - DOCK_H;
    w->no_resize = app->no_resize != 0;
    w->parent_id = g_cur_win ? g_cur_win->id : 0;
    int newidx = g_win_count;
    g_win_count++;
    focus_window(newidx);
    if (app->init) app->init(w->ctx);
    /* scale-in open animation (see draw_window) */
    capture_window_snapshot(w, newidx);
    w->open_t = pit_ticks();
    /* fire the dock launch animation for this app's icon (if it's pinned) */
    for (int i = 0; i < g_app_count; i++)
        if (g_apps[i] == app) {
            int de = dock_find_app(i);
            if (de >= 0) g_dock_launch[de] = pit_ticks();
            break;
        }
}

void gui_open_app_named(const char *name) {
    App *a = gui_find_app(name);
    if (a) gui_open_app(a);
}

/* unregistered image viewer spawned directly by .png double-clicks */
extern "C" App *app_make_photos(void);

/* "Do you want to save?" dialog state: window idx awaiting confirmation,
 * or -1. g_save_dismiss latches once the user picks an action. */
static int g_save_win = -1;
static int g_save_dismiss;   /* 0 none, 1 = save, 2 = don't save, 3 = cancel */
static int g_save_prs = -1;  /* pressed button index while held, -1 none */

/* set by the save dialog's "Don't Save" so confirm_close is bypassed once */
static int g_force_close;

static void draw_window(int idx);   /* defined below; used for snapshots */

/* re-render the window alone into its own pixel buffer so animations
 * (close spin-out, open scale-in) can blit it without baking in whatever
 * windows happen to overlap it on screen. Falls back to horizontal
 * bands when one big contiguous kmalloc fails on a fragmented heap. */
static void capture_window_snapshot(Window *w, int idx) {
    free_window_snapshot(w);   /* drop any stale anim snapshot first */
    w->snap = NULL; w->snap_bands = NULL;
    if (!w->visible || w->minimized || w->w <= 0 || w->h <= 0) return;
    int bw = w->w + 8, bh = w->h + 8;   /* pad for the drop shadow */
    uint32_t *buf = (uint32_t *)kmalloc((size_t)bw * bh * 4);
    if (buf) {
        memset(buf, 0, (size_t)bw * bh * 4);
        uint32_t *oaddr; uint32_t opitch;
        fb_get_target(&oaddr, &opitch);
        int sw = (int)fb_width(), sh = (int)fb_height();
        int ox = w->x, oy = w->y;
        w->x = 0; w->y = 0; w->closing = 0; w->open_t = 0;
        w->min_out = 0; w->zooming = 0;   /* render the REAL frame */
        fb_set_target(buf, (uint32_t)bw * 4);
        fb_set_clip((uint32_t)bw, (uint32_t)bh);
        draw_window(idx);
        fb_set_target(oaddr, opitch);
        fb_set_clip((uint32_t)sw, (uint32_t)sh);
        w->x = ox; w->y = oy;
        w->snap = buf;
        w->snap_w = bw; w->snap_h = bh;
        w->snap_ox = 0; w->snap_oy = 0;
        return;
    }
    /* contiguous snapshot failed - render into horizontal BANDS instead:
     * each band is a separate modest kmalloc, so the animation still
     * works where one big buffer can't fit. */
    const int band_rows = 64;
    int nb = (bh + band_rows - 1) / band_rows;
    uint32_t **bands = (uint32_t **)kmalloc((size_t)nb * sizeof(uint32_t *));
    if (!bands) return;
    int made = 0, ok = 1;
    for (int b = 0; b < nb && ok; b++) {
        int rows = bh - b * band_rows;
        if (rows > band_rows) rows = band_rows;
        bands[b] = (uint32_t *)kmalloc((size_t)bw * rows * 4);
        if (!bands[b]) { ok = 0; break; }
        memset(bands[b], 0, (size_t)bw * rows * 4);
        made++;
        uint32_t *oaddr; uint32_t opitch;
        fb_get_target(&oaddr, &opitch);
        int sw2 = (int)fb_width(), sh2 = (int)fb_height();
        int ox = w->x, oy = w->y;
        w->x = 0; w->y = -b * band_rows; w->closing = 0; w->open_t = 0;
        w->min_out = 0; w->zooming = 0;
        fb_set_target(bands[b], (uint32_t)bw * 4);
        fb_set_clip((uint32_t)bw, (uint32_t)rows);
        draw_window(idx);
        fb_set_target(oaddr, opitch);
        fb_set_clip((uint32_t)sw2, (uint32_t)sh2);
        w->x = ox; w->y = oy;
    }
    if (ok) {
        w->snap_bands = bands;
        w->snap_nb = nb;
        w->snap_band_rows = band_rows;
        w->snap_w = bw; w->snap_h = bh;
        w->snap_ox = 0; w->snap_oy = 0;
    } else {
        for (int b = 0; b < made; b++) kfree(bands[b]);
        kfree(bands);
    }
}

static void free_window_snapshot(Window *w) {
    if (w->snap) { kfree(w->snap); w->snap = NULL; }
    if (w->snap_bands) {
        for (int b = 0; b < w->snap_nb; b++) kfree(w->snap_bands[b]);
        kfree(w->snap_bands); w->snap_bands = NULL;
    }
}

static void close_window_idx(int idx) {
    if (idx < 0 || idx >= g_win_count) return;
    Window *w = &g_win[idx];
    /* app vetoes close (unsaved changes) -> "Do you want to save?" dialog */
    if (!g_force_close && w->app && w->app->confirm_close && w->ctx &&
        w->app->confirm_close(w->ctx)) {
        g_save_win = idx;
        g_save_dismiss = 0;
        return;
    }
    g_force_close = 0;
    w->zooming = 0; w->min_out = 0;   /* cancel any pending anims */
    /* scale-down close: the window shrinks into its center (exact
     * reverse of the open scale-in) instead of vanishing instantly;
     * the sweep in render_frame destroys it once the animation finishes */
    w->closing = 1;
    w->close_t = pit_ticks();
    w->cx0 = w->x; w->cy0 = w->y; w->cw0 = w->w; w->ch0 = w->h;
    /* snapshot the window by RE-RENDERING it alone into its own buffer:
     * copying the composited screen would bake any overlapping windows
     * into the snapshot, making them appear to spin out too. The window
     * is drawn at the origin of a padded buffer (closing flag cleared so
     * draw_window takes its normal path), then the flag/position are
     * restored - only this window's own pixels ever fly away. */
    capture_window_snapshot(w, idx);
    w->closing = 1;   /* capture cleared it for the render pass */
    w->focused = false;
    /* hand focus back to the window that spawned this app first (e.g. the
     * Files window that launched it) - restores it if minimized - so the
     * user never lands on an unrelated instance of the same app. Focus is
     * given IN PLACE (no z-raise): closing a window must not pop another
     * app up over a fullscreen/maximized one - the stack stays as it was */
    if (w->parent_id) {
        for (int i = 0; i < g_win_count; i++) {
            Window *p = &g_win[i];
            if (p->id != w->parent_id || p->closing) continue;
            if (p->minimized) restore_window_spin(i);
            if (p->visible) { focus_window_inplace(i); return; }
            break;
        }
    }
    /* no living parent: focus the topmost-remaining window IN PLACE
     * (highest z, no reordering) so closing never reshuffles the stack */
    int best = -1;
    for (int i = 0; i < g_win_count; i++)
        if (i != idx && g_win[i].visible && !g_win[i].minimized &&
            !g_win[i].closing &&
            (best < 0 || g_win[i].z > g_win[best].z)) best = i;
    if (best >= 0) { focus_window_inplace(best); return; }
    g_focus = -1;
}

static void minimize_window_idx(int idx) {
    if (idx < 0 || idx >= g_win_count) return;
    Window *w = &g_win[idx];
    /* minimize = the same SPIN-OUT as closing, then the window hides.
     * The window stays visible during the ~300ms anim; the draw pass
     * finalizes minimized/focus when it completes */
    if (!w->minimized && w->visible && !w->min_out) {
        w->cx0 = w->x; w->cy0 = w->y; w->cw0 = w->w; w->ch0 = w->h;
        w->min_out = 1;
        w->min_t = pit_ticks();
        capture_window_snapshot(w, idx);
        w->min_out = 1;   /* snapshot render cleared anim flags */
        return;
    }
    w->minimized = true;
    w->visible = false;
    w->focused = false;   /* clear stale focus so dock state is right */
    /* focus next visible */
    for (int i = g_win_count - 1; i >= 0; i--)
        if (g_win[i].visible && !g_win[i].minimized) { focus_window(i); return; }
    g_focus = -1;
}

/* zoom toggle animation: the window's real rect glides to the target
 * (maximized <-> saved frame) with a boing overshoot; draw_window
 * renders it live each frame so apps relayout at every size */
static void zoom_window_anim(int idx, bool to_max) {
    Window *w = &g_win[idx];
    w->zfx = w->x; w->zfy = w->y; w->zfw = w->w; w->zfh = w->h;
    if (to_max) {
        /* remember the exact frame so un-zooming lands back on it */
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->ztx = 0; w->zty = MENU_H;
        w->ztw = (int)fb_width(); w->zth = (int)fb_height() - MENU_H;
    } else {
        if (w->rw > 0) {           /* have a saved frame? use it */
            w->ztx = w->rx; w->zty = w->ry;
            w->ztw = w->rw; w->zth = w->rh;
        } else {
            w->ztx = 80; w->zty = 60;
            w->ztw = w->min_w; w->zth = w->min_h;
        }
    }
    w->zooming = 1;
    w->zoom_t = pit_ticks();
}

/* un-minimize with the SPIN-IN (mirror of spin-out): snapshot the first
 * frame, then grow it back over ~280ms like a freshly opened window */
static void restore_window_spin(int idx) {
    Window *w = &g_win[idx];
    w->minimized = false;
    w->visible = true;
    capture_window_snapshot(w, idx);
    w->open_t = pit_ticks();
}

/* ---------- app drawing helpers (content-relative) ---------- */

int app_content_w(void) { return g_cur_win ? content_w(g_cur_win) : 0; }
int app_content_h(void) { return g_cur_win ? content_h(g_cur_win) : 0; }
int app_content_x(void) { return g_cur_cx; }
int app_content_y(void) { return g_cur_cy; }
int gui_win_focused(void) { return g_cur_win && g_cur_win->focused; }
int gui_win_list_count(void) { return g_win_count; }
int gui_win_list_get(int i, char *app, int acap, char *title, int tcap,
                     int *focused, int *minimized) {
    if (i < 0 || i >= g_win_count) return -1;
    Window *w = &g_win[i];
    if (app && acap > 0) {
        const char *n = (w->app && w->app->name) ? w->app->name : "?";
        strncpy(app, n, (size_t)acap - 1); app[acap - 1] = 0;
    }
    if (title && tcap > 0) {
        const char *t = (w->app && w->app->title) ? w->app->title : "?";
        strncpy(title, t, (size_t)tcap - 1); title[tcap - 1] = 0;
    }
    if (focused) *focused = w->focused ? 1 : 0;
    if (minimized) *minimized = w->minimized ? 1 : 0;
    return w->id;
}

static void app_gx(int x, int *gx) { *gx = g_cur_cx + x; }
static void app_gy(int y, int *gy) { *gy = g_cur_cy + y; }

void app_fill(int x, int y, int w, int h, uint32_t color) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    /* rgb() colors carry alpha=0: treat as opaque. rgba() colors blend. */
    if ((color >> 24) == 0) color |= 0xFF000000u;
    fb_blend_rect(gx, gy, w, h, color);
}
void app_blend(int x, int y, int w, int h, uint32_t color) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_blend_rect(gx, gy, w, h, color);
}
void app_rect(int x, int y, int w, int h, uint32_t color) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_draw_rect(gx, gy, w, h, color);
}
void app_rounded(int x, int y, int w, int h, int rad, uint32_t color) {
    (void)rad;   /* Squarebin: everything is boxy - radius ignored */
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_fill_rect(gx, gy, w, h, color);
}
void app_text(int x, int y, const char *s, uint32_t color, int scale) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_text(gx, gy, s, color, scale);
}
int app_text_clip(int x, int y, const char *s, uint32_t color, int scale,
                  int cx, int cy, int cw, int ch) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    /* clip in content coords -> global */
    int ccx, ccy, ccw, cch;
    app_gx(cx, &ccx); app_gy(cy, &ccy);
    ccw = cw; cch = ch;
    return fb_text_clip(gx, gy, s, color, scale, ccx, ccy, ccw, cch);
}
int app_text_width(const char *s, int scale) { return fb_text_width(s, scale); }
/* fractional-height text for browser zoom (h = glyph px height) */
void app_text_q(int x, int y, const char *s, uint32_t color, int h) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_text_q(gx, gy, s, color, h);
}
int app_text_clip_q(int x, int y, const char *s, uint32_t color, int h,
                    int cx, int cy, int cw, int ch) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    int ccx, ccy; app_gx(cx, &ccx); app_gy(cy, &ccy);
    return fb_text_clip_q(gx, gy, s, color, h, ccx, ccy, cw, ch);
}

void gui_fit_label(char *dst, size_t cap, const char *src, int max_w) {
    char t[160];
    snprintf(t, sizeof(t), "%s", src ? src : "");
    if (fb_text_width(t, 1) <= max_w) {
        snprintf(dst, cap, "%s", t);
        return;
    }
    /* trim chars until "name..." fits the pixel budget */
    size_t n = strlen(t);
    while (n > 1) {
        t[--n] = 0;
        char probe[168];
        snprintf(probe, sizeof(probe), "%s...", t);
        if (fb_text_width(probe, 1) <= max_w) break;
    }
    snprintf(dst, cap, "%s...", t);
}
void app_draw_image(int x, int y, const uint8_t *rgba, int w, int h, int sw, int sh) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_draw_image(gx, gy, w, h, rgba, sw, sh);
}
void app_draw_image_scaled(int x, int y, int w, int h,
                           const uint8_t *rgba, int sw, int sh) {
    int gx, gy; app_gx(x, &gx); app_gy(y, &gy);
    fb_draw_image_scaled(gx, gy, w, h, rgba, sw, sh);
}

/* ---------- wallpaper ---------- */

static uint32_t *g_wallpaper;   /* screen-sized XRGB buffer */
static uint32_t g_wallpaper_w, g_wallpaper_h;
static char g_wallpaper_name[40] = "wp_chrysanthemum";

/* Wallpapers are multi-MB photos - far too big for the shared image cache
 * (8 photos x ~9MB decoded would blow the heap). Decode directly and keep
 * only the LAST used source so switching back is instant. */
static uint8_t *g_wp_src;
static char g_wp_src_name[40];
static uint32_t g_wp_src_w, g_wp_src_h;

static const uint8_t *wallpaper_src(uint32_t *w, uint32_t *h) {
    if (g_wp_src && !strcasecmp(g_wp_src_name, g_wallpaper_name)) {
        *w = g_wp_src_w; *h = g_wp_src_h;
        return g_wp_src;
    }
    size_t sz;
    const uint8_t *png = asset_png(g_wallpaper_name, &sz);
    if (!png) return NULL;
    /* drop the previous decode first: a 1280x960 photo needs ~15MB peak
     * during inflate+unfilter, and holding the old ~5MB rgba on top of
     * that can exhaust the heap -> decode fails -> gradient fallback */
    kfree(g_wp_src);
    g_wp_src = NULL;
    g_wp_src_name[0] = 0;
    __asm__ volatile("cli");
    uint8_t *rgba = NULL;
    uint32_t iw = 0, ih = 0;
    int rc = png_decode(png, sz, &rgba, &iw, &ih);
    __asm__ volatile("sti");
    if (rc || !rgba) return NULL;
    g_wp_src = rgba;
    g_wp_src_w = iw; g_wp_src_h = ih;
    strncpy(g_wp_src_name, g_wallpaper_name, sizeof(g_wp_src_name) - 1);
    *w = iw; *h = ih;
    return rgba;
}

static void wallpaper_build(void) {
    uint32_t sw = fb_width(), sh = fb_height();
    g_wallpaper = (uint32_t *)kmalloc((size_t)sw * sh * 4);
    if (!g_wallpaper) return;
    uint32_t iw, ih;
    const uint8_t *img = wallpaper_src(&iw, &ih);
    if (!img) {
        /* fallback: Sierra-ish gradient (dusk purple -> orange -> dark) */
        for (uint32_t y = 0; y < sh; y++) {
            for (uint32_t x = 0; x < sw; x++) {
                uint32_t t = y * 1000 / sh;   /* fixed-point, no SSE */
                uint8_t r = (uint8_t)(60 + 140 * t / 1000);
                uint8_t g = (uint8_t)(40 + 80 * (1000 - t) / 1000);
                uint8_t b = (uint8_t)(90 + 60 * t / 1000);
                g_wallpaper[(size_t)y * sw + x] = rgb(r, g, b);
            }
        }
        g_wallpaper_w = sw; g_wallpaper_h = sh;
        return;
    }
    /* scale source to screen into u32 buffer */
    for (uint32_t y = 0; y < sh; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * ih / sh);
        if (sy >= ih) sy = ih - 1;
        for (uint32_t x = 0; x < sw; x++) {
            uint32_t sx = (uint32_t)((uint64_t)x * iw / sw);
            if (sx >= iw) sx = iw - 1;
            const uint8_t *p = img + ((size_t)sy * iw + sx) * 4;
            uint32_t c = rgb(p[0], p[1], p[2]);
            g_wallpaper[(size_t)y * sw + x] = c;
        }
    }
    g_wallpaper_w = sw; g_wallpaper_h = sh;
}

const char *gui_wallpaper_name(void) { return g_wallpaper_name; }

void gui_set_wallpaper(const char *name) {
    strncpy(g_wallpaper_name, name, sizeof(g_wallpaper_name) - 1);
    if (g_wallpaper) { kfree(g_wallpaper); g_wallpaper = NULL; }
    wallpaper_build();
}

static void draw_wallpaper(void) {
    if (g_wallpaper) {
        fb_blit_u32(g_wallpaper);
    } else {
        fb_fill_rect(0, 0, fb_width(), fb_height(), rgb(60, 30, 59));
    }
}

/* ---------- menu bar ---------- */

static char g_user_name[64] = "user";
const char *gui_user_name(void) { return g_user_name; }
void gui_set_user_name(const char *name) {
    if (!name || !name[0]) return;
    strncpy(g_user_name, name, sizeof(g_user_name) - 1);
    g_user_name[sizeof(g_user_name) - 1] = 0;
}
uint64_t gui_ticks(void) { return pit_ticks(); }
void gui_request_redraw(void) { /* render loop is continuous */ }
void gui_sleep_ms(int ms) { pit_delay_ms((uint64_t)ms); }

/* menu-bar widget hit rects + state (filled by draw_menu_bar each frame) */
static int g_vol_x0, g_vol_x1, g_vol_y0, g_vol_y1;
static int g_clk_x0, g_clk_x1;
static int g_cal_open, g_cal_x, g_cal_y, g_cal_w, g_cal_h;
static int g_cal_moff;                  /* months away from the real date */
static int g_cal_nav_l[4], g_cal_nav_r[4]; /* ‹ › button rects (x0,y0,x1,y1) */
static int g_vol_drag;

/* boot splash: covers the desktop while the DA BING chime plays so the
 * "Voyager has started" moment is visible, not just audible */
static uint64_t g_splash_start, g_splash_until;

/* Voyager dropdown (Shutdown / Reboot) */
static int g_voy_open, g_voy_x, g_voy_y, g_voy_w, g_voy_h;
static int g_voy_hit[2][4];              /* item rects: Shutdown, Reboot */
static uint64_t g_voy_open_t;            /* slide-in animation start */

/* network status popup (ethernet link / IP) */
static int g_net_open, g_net_x, g_net_y, g_net_w, g_net_h;
static uint64_t g_net_open_t;
static int g_voy_x0, g_voy_x1;           /* "Voyager" word hit rect */
static int g_net_x0, g_net_x1;           /* ethernet icon hit rect */
static uint64_t g_cal_open_t;            /* calendar slide-in start */

/* popup slide-in: returns how many px ABOVE the final position the popup
 * should draw right now (14 -> 0 over ~160ms, ease-out) so menus drop
 * down from the menu bar instead of popping in instantly */
static int popup_slide(uint64_t t0) {
    if (!t0) return 0;
    uint64_t el = pit_ticks() - t0;
    if (el >= 160) return 0;
    int rem = 100 - (int)(el * 100 / 160);      /* 100 -> 0 */
    return (rem * rem * 14) / 10000;
}

/* popup close: slides ALL the way back to its anchor edge (menu bar for
 * top popups, dock for the instance list) like a drawer, then the popup
 * is dropped. ct = close start time, 0 = not closing. dist = px of
 * travel needed to fully hide. Returns px traveled, -1 = finished. */
static uint64_t g_cal_x_t, g_voy_x_t, g_net_x_t, g_pop_x_t;
static int popup_close_rise(uint64_t ct, int dist) {
    if (!ct) return 0;
    uint64_t el = pit_ticks() - ct;
    int dur = 170;
    if (el >= (uint64_t)dur) return -1;
    int t = (int)(el * 100 / dur);
    /* ease-in-out: gentle start and stop, drawer-like */
    int e = t < 50 ? (2 * t * t) / 100
                   : 100 - (2 * (100 - t) * (100 - t)) / 100;
    return (e * dist) / 100;
}
static void popup_start_close(int which) {     /* 0=cal 1=voy 2=net 3=pop */
    uint64_t *xt = which == 0 ? &g_cal_x_t : which == 1 ? &g_voy_x_t :
                   which == 2 ? &g_net_x_t : &g_pop_x_t;
    if (!*xt) *xt = pit_ticks();
}

static void draw_menu_bar(App *focused_app) {
    uint32_t sw = fb_width();
    /* drop shadow falling below the bar - same rgba(0,0,0) family the
     * windows and dialogs cast, half thickness */
    fb_blend_rect(0, MENU_H, sw, 6, rgba(0, 0, 0, 105));
    /* Voyager theme: deep space navy bar with starlight text */
    fb_blend_rect(0, 0, sw, MENU_H, rgba(44, 13, 48, 205));
    fb_fill_rect(0, MENU_H - 1, sw, 1, rgb(126, 58, 140));
    fb_blend_rect(0, 0, sw, 1, rgba(228, 120, 255, 60));

    int x = 10;
    /* Voyager planet logo - plain like desktop icons: no hover wash,
     * the hand cursor alone marks it clickable */
    asset_draw(x, 4, 16, "squarebin");
    x += 20;
    fb_text(x, 8, "Voyager", rgb(250, 235, 255), 1);
    g_voy_x0 = x - 4; g_voy_x1 = x + fb_text_width("Voyager", 1) + 10;
    x += fb_text_width("Voyager", 1) + 14;
    if (focused_app) {
        fb_text(x, 8, focused_app->name, rgb(216, 170, 230), 1);
    }
    /* right side: user name + ethernet icon + volume slider + clock.
     * Laid out right-to-left with ONE even gap between every element so
     * nothing crams together regardless of name/clock widths. */
    char clock[32];
    rtc_get_time(clock, sizeof(clock));
    int cw = fb_text_width(clock, 1);
    int clk_x = (int)sw - cw - 12;
    int uw = fb_text_width(g_user_name, 1);
    const int GAP = 16;

    /* volume: speaker glyph + slider, vertically centered in the 24px bar */
    int vol_w = 56;
    int vol_x0 = clk_x - GAP - vol_w;
    int vol_y = 6;                  /* track center lands on y=12.5 = bar mid */
    int spk_x = vol_x0 - 6 - 13;    /* speaker glyph is ~13px wide */

    /* ethernet status icon left of the speaker */
    int net_x = spk_x - GAP - 17;
    bool link = e1000_link_up();
    uint32_t nic = link ? rgb(90, 220, 140) : rgb(110, 90, 110);
    /* little monitor glyph: screen + stand, link LED dot */
    fb_fill_rect(net_x, 5, 14, 10, nic);
    fb_fill_rect(net_x + 2, 7, 10, 6, rgb(44, 13, 48));
    fb_fill_rect(net_x + 6, 15, 2, 3, nic);
    fb_fill_rect(net_x + 4, 18, 6, 1, nic);
    fb_fill_rect(net_x + 12, 4, 3, 3, link ? rgb(120, 255, 160) : rgb(70, 70, 80));
    g_net_x0 = net_x - 3; g_net_x1 = net_x + 17;

    fb_text(net_x - GAP - uw, 8, g_user_name, rgb(216, 170, 230), 1);
    int vp = ac97_volume();
    /* speaker: box + cone always, centered on the slider midline (y=12.5);
     * wave bars appear with volume, and at 0% a red X crosses the glyph */
    fb_fill_rect(spk_x, 9, 4, 7, rgb(216, 170, 230));       /* box  9-16 */
    fb_fill_rect(spk_x + 4, 7, 3, 11, rgb(216, 170, 230));  /* cone 7-18 */
    fb_fill_rect(spk_x + 7, 10, 2, 5, rgb(216, 170, 230));  /* stem 10-15 */
    if (vp >= 15) fb_fill_rect(spk_x + 10, 8, 1, 9, rgb(148, 90, 255));
    if (vp >= 55) fb_fill_rect(spk_x + 13, 6, 1, 13, rgb(148, 90, 255));
    if (vp <= 0) {
        /* muted: red X across the speaker */
        for (int i = 0; i < 10; i++) {
            fb_fill_rect(spk_x + i, 6 + i, 2, 2, rgb(235, 70, 70));
            fb_fill_rect(spk_x + 12 - i, 6 + i, 2, 2, rgb(235, 70, 70));
        }
    }
    /* slider track + fill + knob */
    fb_fill_rect(vol_x0, vol_y + 4, vol_w, 5, rgb(79, 38, 88));
    int fw = vp * vol_w / 100;
    if (fw > 0) fb_fill_rect(vol_x0, vol_y + 4, fw, 5, rgb(128, 90, 255));
    int kx = vol_x0 + fw - 2; if (kx < vol_x0) kx = vol_x0;
    fb_fill_rect(kx, vol_y + 1, 4, 11, rgb(246, 230, 255));
    fb_text(clk_x, 8, clock, rgb(216, 170, 230), 1);

    /* publish hit rects for the input handler */
    g_vol_x0 = vol_x0 - 3; g_vol_x1 = vol_x0 + vol_w + 3;
    g_vol_y0 = vol_y - 3;  g_vol_y1 = vol_y + 15;
    g_clk_x0 = clk_x - 4;  g_clk_x1 = clk_x + cw + 4;
}

/* calendar popup under the clock - click clock to toggle */
static void draw_calendar(void) {
    if (!g_cal_open) return;
    int W = 208, H = 168;
    int cr = popup_close_rise(g_cal_x_t, MENU_H + H + 4);
    if (cr < 0) { g_cal_open = 0; g_cal_x_t = 0; return; }
    int ry, rm, rd, rwd;
    rtc_date_parts(&ry, &rm, &rd, &rwd);
    /* displayed month = real month + g_cal_moff (wheel / ‹ › navigate) */
    int total = ry * 12 + (rm - 1) + g_cal_moff;
    int yy = total / 12, mm = total % 12 + 1;
    if (mm < 1) { mm += 12; yy--; }
    int dd = rd, wd = rwd;
    static const char *mon_names[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    static const char *wd_names[] = { "Su","Mo","Tu","We","Th","Fr","Sa" };
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[mm - 1];
    if (mm == 2 && (yy % 4 == 0 && (yy % 100 || yy % 400 == 0))) dim = 29;

    int px = (int)fb_width() - W - 10;
    /* while closing, the open slide-in is frozen so the two animations
     * can't fight each other (that was the up/down jitter) */
    int sl = g_cal_x_t ? 0 : popup_slide(g_cal_open_t);
    int py = MENU_H + 2 - sl - cr;
    g_cal_x = px; g_cal_y = py; g_cal_w = W; g_cal_h = H;
    /* ‹ › month-nav buttons flanking the title */
    int nl_x = px + 8, nr_x = px + W - 26;
    g_cal_nav_l[0] = nl_x;       g_cal_nav_l[1] = py + 4;
    g_cal_nav_l[2] = nl_x + 18;  g_cal_nav_l[3] = py + 24;
    g_cal_nav_r[0] = nr_x;       g_cal_nav_r[1] = py + 4;
    g_cal_nav_r[2] = nr_x + 18;  g_cal_nav_r[3] = py + 24;
    bool hnl = g_mx >= nl_x && g_mx < nl_x + 18 &&
               g_my >= py + 4 && g_my < py + 24;
    bool hnr = g_mx >= nr_x && g_mx < nr_x + 18 &&
               g_my >= py + 4 && g_my < py + 24;
    if (hnl) fb_blend_rect(nl_x, py + 4, 18, 20, rgba(150, 70, 170, 160));
    if (hnr) fb_blend_rect(nr_x, py + 4, 18, 20, rgba(150, 70, 170, 160));
    fb_text(nl_x + 5, py + 8, "<", rgb(248, 230, 255), 1);
    fb_text(nr_x + 5, py + 8, ">", rgb(248, 230, 255), 1);
    /* drop shadow - same offset family as the other popups */
    fb_blend_rect(px + 4, py + 4, W, H, rgba(0, 0, 0, 90));
    /* translucent like the menu bar/popups, not a solid slab */
    fb_blend_rect(px, py, W, H, rgba(45, 15, 50, 215));
    fb_fill_rect(px, py, W, 1, rgb(150, 70, 170));
    fb_fill_rect(px, py + H - 1, W, 1, rgb(150, 70, 170));
    fb_fill_rect(px, py, 1, H, rgb(150, 70, 170));
    fb_fill_rect(px + W - 1, py, 1, H, rgb(150, 70, 170));

    char title[48];
    snprintf(title, sizeof(title), "%s %d", mon_names[mm - 1], yy);
    int tw = fb_text_width(title, 1);
    fb_text(px + (W - tw) / 2, py + 8, title, rgb(248, 230, 255), 1);

    int gx = px + 12, gy = py + 30, cw2 = 26, ch2 = 20;
    for (int i = 0; i < 7; i++)
        fb_text(gx + i * cw2 + 8, gy, wd_names[i], rgb(171, 120, 190), 1);
    gy += 20;
    /* first weekday of the 1st: CMOS weekday is for TODAY; compute offset
     * by stepping back (dd-1) days modulo 7 */
    int first_wd = (wd - (dd - 1)) % 7; if (first_wd < 0) first_wd += 7;
    for (int d = 1; d <= dim; d++) {
        int cell = first_wd + d - 1;
        int cx = gx + (cell % 7) * cw2;
        int cy = gy + (cell / 7) * ch2;
        char nb[4]; snprintf(nb, sizeof(nb), "%d", d);
        int nw = fb_text_width(nb, 1);
        if (g_cal_moff == 0 && d == dd) {
            fb_fill_rect(cx + 3, cy + 2, cw2 - 6, ch2 - 4, rgb(173, 70, 230));
            fb_text(cx + (cw2 - nw) / 2, cy + 5, nb, rgb(255, 255, 255), 1);
        } else {
            fb_text(cx + (cw2 - nw) / 2, cy + 5, nb, rgb(232, 205, 240), 1);
        }
    }
}

/* calendar panel click: only the ‹ › month-nav buttons do anything;
 * every click inside the panel is swallowed by the caller so it can
 * never fall through to windows/icons drawn beneath the overlay. */
static void calendar_click(int mx, int my) {
    if (mx >= g_cal_nav_l[0] && mx < g_cal_nav_l[2] &&
        my >= g_cal_nav_l[1] && my < g_cal_nav_l[3]) {
        if (g_cal_moff > -1200) g_cal_moff--;
    } else if (mx >= g_cal_nav_r[0] && mx < g_cal_nav_r[2] &&
               my >= g_cal_nav_r[1] && my < g_cal_nav_r[3]) {
        if (g_cal_moff < 1200) g_cal_moff++;
    }
}

/* ---------- Voyager dropdown + network popup + boot splash ---------- */

static void draw_voyager_menu(void) {
    if (!g_voy_open) return;
    int vr = popup_close_rise(g_voy_x_t, MENU_H + 74 + 4);
    if (vr < 0) { g_voy_open = 0; g_voy_x_t = 0; return; }
    int w = 170, h = 74;
    /* anchor to the STABLE menu-bar hit rect (g_voy_x0) - using g_voy_x
     * here fed the popup's own previous x back in, drifting it 4px left
     * every frame until it slid off screen */
    int x = g_voy_x0 - 4;
    int y = MENU_H + 2 - (g_voy_x_t ? 0 : popup_slide(g_voy_open_t)) - vr;
    if (x + w > (int)fb_width()) x = (int)fb_width() - w - 4;
    g_voy_x = x; g_voy_y = y; g_voy_w = w; g_voy_h = h;
    fb_blend_rect(x + 2, y + 3, w, h, rgba(0, 0, 0, 90));
    fb_blend_rect(x, y, w, h, rgba(47, 16, 52, 235));
    fb_fill_rect(x, y, w, 1, rgb(142, 70, 160));
    fb_fill_rect(x, y + h - 1, w, 1, rgb(142, 70, 160));
    fb_text(x + 10, y + 8, "Voyager", rgb(250, 235, 255), 1);
    const char *items[2] = { "Shutdown", "Reboot" };
    for (int i = 0; i < 2; i++) {
        int iy = y + 26 + i * 22;   /* 8px title, even 8px panel margins */
        bool hov = g_mx >= x + 4 && g_mx < x + w - 4 &&
                   g_my >= iy && g_my < iy + 18;
        if (hov) fb_blend_rect(x + 4, iy, w - 8, 18, rgba(218, 90, 255, 60));
        fb_text(x + 12, iy + 4, items[i],
                hov ? rgb(240, 246, 255) : rgb(226, 190, 235), 1);
        g_voy_hit[i][0] = x + 4; g_voy_hit[i][1] = iy;
        g_voy_hit[i][2] = x + w - 4; g_voy_hit[i][3] = iy + 18;
    }
}

static void draw_net_popup(void) {
    if (!g_net_open) return;
    int nr = popup_close_rise(g_net_x_t, MENU_H + 60 + 4);
    if (nr < 0) { g_net_open = 0; g_net_x_t = 0; return; }
    int w = 190, h = 60;
    int x = g_net_x0 - 10;
    int y = MENU_H + 2 - (g_net_x_t ? 0 : popup_slide(g_net_open_t)) - nr;
    if (x + w > (int)fb_width()) x = (int)fb_width() - w - 4;
    g_net_x = x; g_net_y = y; g_net_w = w; g_net_h = h;
    fb_blend_rect(x + 2, y + 3, w, h, rgba(0, 0, 0, 90));
    fb_blend_rect(x, y, w, h, rgba(47, 16, 52, 235));
    fb_fill_rect(x, y, w, 1, rgb(142, 70, 160));
    fb_fill_rect(x, y + h - 1, w, 1, rgb(142, 70, 160));
    fb_text(x + 10, y + 8, "Network", rgb(250, 235, 255), 1);
    if (e1000_link_up()) {
        fb_text(x + 10, y + 27, "Ethernet: Connected", rgb(140, 230, 170), 1);
        uint32_t ip = net_ip();
        if (ip) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (ip >> 24) & 255, (ip >> 16) & 255,
                     (ip >> 8) & 255, ip & 255);
            fb_text(x + 10, y + 45, buf, rgb(216, 170, 230), 1);
        } else {
            fb_text(x + 10, y + 45, "no IP yet", rgb(186, 150, 195), 1);
        }
    } else {
        fb_text(x + 10, y + 27, "Ethernet: No cable", rgb(235, 120, 120), 1);
        fb_text(x + 10, y + 45, "No Wi-Fi hardware", rgb(186, 150, 195), 1);
    }
}

static void draw_splash(void) {
    int w = fb_width(), h = fb_height();
    /* deep-space backdrop with a deterministic starfield */
    fb_fill_rect(0, 0, w, h, rgb(21, 6, 24));
    for (int i = 0; i < 160; i++) {
        int sx = (int)((i * 73u + 11u) % (uint32_t)w);
        int sy = (int)((i * 131u + 7u) % (uint32_t)h);
        uint32_t b = 90 + ((i * 37u) % 130);
        fb_fill_rect(sx, sy, 1 + (i & 1), 1 + (i & 1), rgb(b / 2 + 40, b, 220));
    }
    int ls = 110;
    asset_draw(w / 2 - ls / 2, h / 2 - ls / 2 - 34, ls, "squarebin");
    const char *t = "Voyager";
    int tw = fb_text_width(t, 3);
    fb_text(w / 2 - tw / 2, h / 2 + 48, t, rgb(250, 235, 255), 3);
    /* progress bar synced to the ~2.9s chime so the bar completes right
     * as the bell resolves */
    int bw = w / 3, bx = w / 2 - bw / 2, by = h / 2 + 92;
    fb_fill_rect(bx, by, bw, 5, rgb(73, 30, 82));
    uint64_t dur = g_splash_until - g_splash_start;
    uint64_t el = pit_ticks() - g_splash_start;
    int pw = dur ? (int)(el * bw / dur) : bw;
    if (pw > bw) pw = bw;
    fb_fill_rect(bx, by, pw, 5, rgb(149, 96, 255));
    fb_fill_rect(bx, by, bw, 1, rgb(53, 20, 60));
    const char *sub = "Starting up...";
    fb_text(w / 2 - fb_text_width(sub, 1) / 2, by + 16,
            sub, rgb(186, 140, 200), 1);
}

/* ---------- dock ---------- */

/* shared dock layout: which entries are visible, their magnified sizes and
 * screen positions. Used by BOTH draw and hit-test so clicks land right. */
struct DockLay {
    int first, vis, overflow;
    int dock_x, dock_y, dock_cy, total_w;
    int btn_l, btn_r;      /* < > button x ranges when overflow (w=30) */
    int w[DOCK_MAX], ix[DOCK_MAX], iy[DOCK_MAX];
};

/* auto-hide state: while the frontmost window is maximized the dock packs
 * itself off the bottom edge; g_dock_peek latches it back while the mouse
 * is near the bottom bezel (with hysteresis so it doesn't flicker) */
static int g_dock_peek;
static bool dock_auto_hidden(void) {
    int top = -1;
    for (int i = 0; i < g_win_count; i++)
        if (g_win[i].visible && !g_win[i].minimized && !g_win[i].closing &&
            (top < 0 || g_win[i].z > g_win[top].z)) top = i;
    return top >= 0 && g_win[top].maximized;
}

static void dock_layout(int mx, int my, struct DockLay *L) {
    uint32_t sw = fb_width(), sh = fb_height();
    memset(L, 0, sizeof(*L));
    if (g_dock_n <= 0) return;
    int slot = DOCK_ICON + DOCK_PAD;
    int max_w = (int)sw - 40;
    int base_total = g_dock_n * slot + DOCK_PAD;
    L->overflow = base_total > max_w;
    int avail = L->overflow ? max_w - 76 : max_w;
    /* how many entries fit at base size in the available width? */
    int fit = 0, acc = DOCK_PAD;
    for (int i = 0; i < g_dock_n; i++) {
        if (acc + slot > avail && fit > 0) break;
        acc += slot; fit++;
    }
    if (fit < 1) fit = 1;
    int max_scroll = g_dock_n - fit; if (max_scroll < 0) max_scroll = 0;
    if (g_dock_scroll > max_scroll) g_dock_scroll = max_scroll;
    if (g_dock_scroll < 0) g_dock_scroll = 0;
    L->first = g_dock_scroll;
    L->vis = g_dock_n - L->first; if (L->vis > fit) L->vis = fit;
    g_dock_vis = L->vis;

    L->dock_y = (int)sh - DOCK_H - 16;
    /* front window maximized -> dock slides down but leaves a ~12px sliver
     * of the bar peeked above the bottom edge (so users can see it's
     * there). It slides back up ONLY when the pointer actually TOUCHES
     * that sliver within the dock's horizontal span - not when it is
     * merely close to the bottom edge elsewhere. Once revealed it stays
     * up while the pointer remains over the dock's footprint, and slides
     * back down when the pointer clearly leaves it. */
    int dock_target = L->dock_y;
    if (dock_auto_hidden()) {
        int bw = g_dock_n * slot + DOCK_PAD + (L->overflow ? 76 : 0);
        int fx0 = (int)((sw - bw) / 2) - 16;
        int fx1 = (int)((sw + bw) / 2) + 16;
        /* reveal ONLY when the pointer actually touches the bottom
         * edge within the dock's horizontal span - the dock hides
         * completely (no sliver) so the desktop stays clean. Once up,
         * keep it while the pointer stays over the dock's footprint;
         * hide when it clearly leaves upward/sideways */
        bool touch = mx >= fx0 && mx <= fx1 && my >= (int)sh - 30;
        if (!g_dock_peek && touch) g_dock_peek = 1;
        else if (g_dock_peek &&
                 (my < (int)sh - DOCK_H - 16 - 56 ||
                  mx < fx0 - 48 || mx > fx1 + 48))
            g_dock_peek = 0;
        dock_target = g_dock_peek ? L->dock_y : (int)sh;
    } else g_dock_peek = 0;
    {
        static int cur_y = -1;
        static uint64_t last_t;
        uint64_t now = pit_ticks();
        if (cur_y < 0) { cur_y = dock_target; last_t = now; }
        /* ~0.22px/ms -> the ~68px slide takes ~300ms, visible but quick */
        int step = (int)((now - last_t) * 22 / 100);
        last_t = now;
        if (step < 1) step = 1;
        if (cur_y < dock_target) { cur_y += step; if (cur_y > dock_target) cur_y = dock_target; }
        else if (cur_y > dock_target) { cur_y -= step; if (cur_y < dock_target) cur_y = dock_target; }
        L->dock_y = cur_y;
    }
    L->dock_cy = L->dock_y + DOCK_H / 2;

    /* genie magnification field (same math as before, per visible entry).
     * While the dock is auto-hidden and not peeked, NO magnification -
     * the sliver stays flat and inert until the pointer actually touches
     * it, so icons don't "balloon" near the bottom edge of a maximized
     * window */
    int ady = my - L->dock_cy; if (ady < 0) ady = -ady;
    int vf = 300 - ady * 2;
    if (vf < 0) vf = 0; if (vf > 300) vf = 300;
    /* magnification/hit while the pointer is in the dock's approach
     * zone: inside the bar band plus ~72px above it (the genie swells
     * as you near) and within the bar's horizontal span. The zone is
     * deliberately generous so a normal reach toward the dock reacts,
     * while roaming the upper desktop still does nothing. */
    int bw0 = L->vis * slot + DOCK_PAD;
    int bx0 = (int)((sw - bw0) / 2);
    /* generous band: ~72px above the bar AND all the way to the screen's
     * bottom edge - the genie reacts whenever the pointer crosses the
     * dock's border zone, not only when it lands on the pixels */
    if (my < L->dock_y - 72 ||
        mx < bx0 - 30 || mx > bx0 + bw0 + 30) vf = 0;
    if (dock_auto_hidden() && !g_dock_peek) vf = 0;
    const int MAX_EXTRA = 30, RANGE = 130;
    int base_w = L->vis * slot + DOCK_PAD;
    int base_x = (int)((sw - base_w) / 2);
    for (int j = 0; j < L->vis; j++) {
        int cx = base_x + DOCK_PAD / 2 + j * slot + DOCK_ICON / 2;
        int dx = mx - cx; if (dx < 0) dx = -dx;
        int e = 0;
        if (vf && dx < RANGE) {
            int64_t t = RANGE - dx;
            int64_t u = t * 1024 / RANGE;
            int64_t s = (u * u * (3072 - 2 * u)) / (1024LL * 1024);
            e = (int)((int64_t)MAX_EXTRA * vf * s / (300LL * 1024));
        }
        L->w[j] = DOCK_ICON + e;
    }
    /* when overflowing, reserve a 38px arrow zone at EACH end of the bar
     * so the < > buttons can never overlap the first/last icon */
    int arrow_res = L->overflow ? 38 : 0;
    L->total_w = DOCK_PAD + arrow_res;
    for (int j = 0; j < L->vis; j++) L->total_w += L->w[j] + 10;
    L->total_w += DOCK_PAD + arrow_res;
    L->dock_x = (int)((sw - L->total_w) / 2);
    if (L->dock_x < 4) L->dock_x = 4;
    /* scroll buttons live inside the bar ends */
    L->btn_l = L->dock_x + 6;
    L->btn_r = L->dock_x + L->total_w - 36;

    int pen = L->dock_x + DOCK_PAD / 2 + arrow_res;
    for (int j = 0; j < L->vis; j++) {
        int ent = L->first + j;
        L->ix[j] = pen;
        /* bottom-anchored: magnified icons grow upward like the genie */
        L->iy[j] = L->dock_y + DOCK_H - 13 - L->w[j];
        g_dock_cx[ent] = pen + L->w[j] / 2;
        g_dock_iy[ent] = L->iy[j];
        pen += L->w[j] + 10;
    }
}

/* case-insensitive ".png" suffix check - any name ending in .png/.PNG/.Png */
int gui_is_png(const char *name) {
    if (!name) return 0;
    size_t l = strlen(name);
    if (l < 5) return 0;
    const char *e = name + l - 4;
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') &&
           (e[2] == 'n' || e[2] == 'N') && (e[3] == 'g' || e[3] == 'G');
}

/* image for a dock entry: apps use their icon asset, VFS .png files fly
 * their real pixels, everything else the themed file/folder icon */
static const uint8_t *dock_ent_image(struct DockEnt *e, uint32_t *w, uint32_t *h) {
    if (e->kind == 0) return image_data(g_apps[e->app]->name, w, h);
    if (gui_is_png(e->name)) {
        const uint8_t *im = gui_vfs_thumb(e->dir, e->name, w, h);
        if (im) return im;
    }
    const char *ic = e->name[0] ? gui_file_icon(e->name) : "unknown";
    /* folders keep the folder icon even if named like a file */
    int fi = e->dir[0] ? vfs_find_in(e->dir, e->name) : vfs_find(e->name);
    const VfsEntry *ve = fi >= 0 ? vfs_entry(fi) : NULL;
    if (ve && ve->type == VFS_DIR) ic = "files";
    return image_data(ic, w, h);
}

static void draw_dock(int mouse_x, int mouse_y) {
    struct DockLay L;
    dock_layout(mouse_x, mouse_y, &L);
    if (g_dock_n <= 0 || L.vis <= 0) return;
    uint32_t sw = fb_width();
    (void)sw;

    /* drop shadow on the bottom + right edges, half thickness, corner
     * connected - same rgba(0,0,0) family as the menu bar and windows */
    fb_blend_rect(L.dock_x + L.total_w, L.dock_y, 6, DOCK_H + 6,
                  rgba(0, 0, 0, 105));
    fb_blend_rect(L.dock_x, L.dock_y + DOCK_H, L.total_w, 6,
                  rgba(0, 0, 0, 105));

    /* premium dock bar: translucent Voyager navy, starlight hairline,
     * soft floor shadow - same family as menu bar and popups */
    fb_blend_rect(L.dock_x, L.dock_y, L.total_w, DOCK_H, rgba(44, 13, 48, 185));
    fb_blend_rect(L.dock_x, L.dock_y, L.total_w, 2, rgba(232, 140, 255, 70));
    fb_blend_rect(L.dock_x, L.dock_y + DOCK_H - 2, L.total_w, 2, rgba(0, 0, 0, 70));
    /* no strip under the dock - it floats clean on the desktop */

    /* scroll buttons when the dock overflows */
    if (L.overflow) {
        int by = L.dock_y + (DOCK_H - 30) / 2, bh = 30;
        bool hl = (mouse_x >= L.btn_l && mouse_x < L.btn_l + 30 &&
                   mouse_y >= by && mouse_y < by + bh);
        bool hr = (mouse_x >= L.btn_r && mouse_x < L.btn_r + 30 &&
                   mouse_y >= by && mouse_y < by + bh);
        fb_blend_rounded(L.btn_l, by, 30, bh, 6, hl ? rgba(150, 70, 170, 200) : rgba(73, 30, 80, 200));
        fb_blend_rounded(L.btn_r, by, 30, bh, 6, hr ? rgba(150, 70, 170, 200) : rgba(73, 30, 80, 200));
        fb_draw_rect(L.btn_l, by, 30, bh, rgb(182, 96, 200));
        fb_draw_rect(L.btn_r, by, 30, bh, rgb(182, 96, 200));
        fb_text(L.btn_l + 11, by + 8, "<", rgb(249, 232, 255), 1);
        fb_text(L.btn_r + 11, by + 8, ">", rgb(249, 232, 255), 1);
    }

    for (int j = 0; j < L.vis; j++) {
        int ent = L.first + j;
        struct DockEnt *de = &g_dock[ent];
        int dw = L.w[j], dh = dw;   /* uniform scale: magnify without stretching */
        int ix = L.ix[j], iy = L.iy[j];
        /* launch animation: the icon rockets up off the dock and settles
         * back with a soft bounce - Voyager taking off (~1.1s).
         * Integer parabolas (no libm in freestanding). */
        if (g_dock_launch[ent]) {
            uint64_t dt = pit_ticks() - g_dock_launch[ent];
            if (dt < 1150) {
                int p = (int)(dt * 1000 / 1150);      /* 0..999 */
                int hop = 120 * p * (1000 - p) / 1000000;   /* ~30px peak */
                if (p > 600) {                        /* small 2nd bounce */
                    int q = (p - 600) * 1000 / 400;
                    hop += 36 * q * (1000 - q) / 1000000;
                }
                iy -= hop;
            } else g_dock_launch[ent] = 0;
        }
        uint32_t iw = 0, ih = 0;
        const uint8_t *img = dock_ent_image(de, &iw, &ih);
        if (img)
            fb_draw_image_scaled(ix, iy, dw, dh, img, (int)iw, (int)ih);
        int cx = ix + dw / 2;
        /* taskbar arrow under the icon with a clear gap: vivid blue = focused,
         * pale = open, darker slate = every window minimized (apps only) */
        int cnt = de->kind == 0 ? app_window_count(g_apps[de->app]) : 0;
        if (cnt > 0) {
            uint32_t ic;
            if (app_focused_idx(g_apps[de->app]) >= 0) ic = rgb(134, 38, 160);
            else if (app_all_minimized(g_apps[de->app])) ic = rgb(138, 70, 150);
            else ic = rgb(199, 168, 208);
            int ay = L.dock_y + DOCK_H - 8;
            for (int r = 0; r < 5; r++) {
                int hw = r;                 /* point-up triangle */
                fb_fill_rect(cx - hw, ay + r, hw * 2 + 1, 1, ic);
            }
        }
        /* instance count: bare number, no box (dark shadow + light glyph) */
        if (cnt > 1) {
            char nb[4];
            nb[0] = (char)('0' + (cnt > 9 ? 9 : cnt)); nb[1] = 0;
            int nx = ix + dw - 11, ny = iy + 1;
            fb_text(nx + 1, ny + 1, nb, rgb(10, 10, 20), 1);
            fb_text(nx, ny, nb, rgb(240, 244, 255), 1);
        }
    }
    /* dock-icon context menu: apps get [Close All Windows] +
     * [Remove from Dock]; VFS entries get Remove only */
    if (g_dock_menu >= 0) {
        int n = g_dock_menu_app ? 2 : 1;
        int mw = 150, mh = 8 + n * 24;
        int x = g_dock_menu_x, y = g_dock_menu_y;
        if (x + mw > (int)sw - 4) x = (int)sw - mw - 4;
        if (y + mh > L.dock_y - 4) y = L.dock_y - mh - 4;
        fb_blend_rect(x + 3, y + 3, mw, mh, rgba(0, 0, 0, 80));
        fb_blend_rect(x, y, mw, mh, rgba(44, 13, 48, 235));
        fb_draw_rect(x, y, mw, mh, rgb(182, 96, 200));
        fb_blend_rect(x, y, mw, 2, rgba(233, 130, 255, 130));
        static const char *ritem[] = { "Close All Windows", "Remove from Dock" };
        for (int i = 0; i < n; i++) {
            int ry = y + 4 + i * 24;
            bool hov = (mouse_x >= x + 4 && mouse_x < x + mw - 4 &&
                        mouse_y >= ry && mouse_y < ry + 22);
            if (hov) fb_blend_rect(x + 4, ry, mw - 8, 22, rgba(150, 70, 170, 150));
            fb_text_clip(x + 12, ry + 7, ritem[i], rgb(249, 232, 255), 1,
                         x + 6, ry, mw - 12, 20);
        }
        g_dock_menu_x = x; g_dock_menu_y = y;
        g_dock_menu_w = mw; g_dock_menu_h = mh;
    }
}

/* hit test dock icon -> ENTRY index, -1 none, -2 prev button, -3 next button.
 * Mirrors dock_layout so clicks land on magnified/moved icons too. */
static int dock_hit(int mx, int my) {
    struct DockLay L;
    dock_layout(mx, my, &L);
    if (g_dock_n <= 0 || L.vis <= 0) return -1;
    /* dock auto-hidden and not peeked -> completely inert: no icon hits,
     * no hand cursor, no clicks. Only a real touch on the sliver (which
     * sets g_dock_peek inside dock_layout) brings it to life */
    if (dock_auto_hidden() && !g_dock_peek) return -1;
    /* same approach band as magnification: generous reach above the
     * bar so near-touches still register, but not from across screen */
    if (my < L.dock_y - 72 || my >= L.dock_y + DOCK_H + 4) return -1;
    if (L.overflow) {
        int by = L.dock_y + (DOCK_H - 30) / 2;
        if (mx >= L.btn_l && mx < L.btn_l + 30 && my >= by && my < by + 30)
            return -2;
        if (mx >= L.btn_r && mx < L.btn_r + 30 && my >= by && my < by + 30)
            return -3;
    }
    if (mx < L.dock_x || mx >= L.dock_x + L.total_w) return -1;
    for (int j = 0; j < L.vis; j++) {
        /* generous approach zone for the CURSOR/magnification, but a
         * click only counts on the icon itself (plus a few px) */
        if (mx >= L.ix[j] - 4 && mx < L.ix[j] + L.w[j] + 4 &&
            my >= L.iy[j] - 6 && my < L.dock_y + DOCK_H + 4)
            return L.first + j;
    }
    return -1;
}


/* ---------- instance popup (Windows-10-style window list) ---------- */

static int g_pop_app = -1;              /* dock app index showing its window list */
static int g_pop_dock = -1;             /* dock ENTRY index (for icon position) */
static int g_pop_item[MAX_WINDOWS];     /* window indices, one per row */
static int g_pop_rows;
static int g_pop_scroll;                /* first visible row when list is long */
static int g_pop_x, g_pop_y, g_pop_w, g_pop_h;
#define POP_VIS 6                        /* max rows visible before scrolling */

static void draw_instance_popup(void) {
    if (g_pop_app < 0 || g_pop_app >= g_app_count) { g_pop_app = -1; return; }
    App *a = g_apps[g_pop_app];
    /* rebuild rows every frame: windows may close while the list is open */
    int rows[MAX_WINDOWS]; int nr = 0;
    for (int i = 0; i < g_win_count && nr < MAX_WINDOWS; i++)
        if (g_win[i].app == a) rows[nr++] = i;
    if (nr == 0) { g_pop_app = -1; g_pop_x_t = 0; return; }

    uint32_t sw = fb_width(), sh = fb_height();
    int dock_y = (int)sh - DOCK_H - 16;
    int vis = nr < POP_VIS ? nr : POP_VIS;
    /* width hugs the content: from the color-chip zone to just past the
     * longest row text, with the same padding gaps as before */
    int maxw = fb_text_width(a->name, 1);
    for (int i = 0; i < vis; i++) {
        char t[64];
        snprintf(t, sizeof(t), "%s #%d",
                 a->title ? a->title : a->name, g_pop_scroll + i + 1);
        int tw = fb_text_width(t, 1);
        if (tw > maxw) maxw = tw;
    }
    int w = 26 + maxw + 14;      /* chip zone + text + right gap */
    if (w < 120) w = 120;
    if (w > 260) w = 260;
    int h = 30 + vis * 24;
    /* scroll clamp: keep g_pop_scroll inside [0, nr-vis] */
    int maxs = nr - vis;
    if (g_pop_scroll > maxs) g_pop_scroll = maxs;
    if (g_pop_scroll < 0) g_pop_scroll = 0;
    /* align the popup's center over the dock icon that opened it */
    int icx = (g_pop_dock >= 0 && g_pop_dock < DOCK_MAX) ? g_dock_cx[g_pop_dock]
                                                    : (int)(sw / 2);
    int x = icx - w / 2;
    if (x < 4) x = 4;
    if (x + w > (int)sw - 4) x = (int)sw - w - 4;
    /* rise with the icon: popup bottom hugs the icon's top edge, so it
     * moves up/down as the dock icon balloons vertically (and stays centered
     * on it horizontally via g_dock_cx) */
    int icon_top = (g_pop_dock >= 0 && g_pop_dock < DOCK_MAX) ? g_dock_iy[g_pop_dock]
                                                       : dock_y + DOCK_H - 60;
    int y = icon_top - h - 6;
    int ymax = dock_y - h - 8;          /* never sink into the dock */
    if (y > ymax) y = ymax;
    if (y < MENU_H + 4) y = MENU_H + 4;
    /* closing? slide DOWN off the bottom edge (the dock side) until the
     * popup is fully hidden - same drawer feel as the menu popups */
    int pr = popup_close_rise(g_pop_x_t, (int)sh - (y + h) + 8);
    if (pr < 0) { g_pop_app = -1; g_pop_x_t = 0; return; }
    y += pr;
    g_pop_x = x; g_pop_y = y; g_pop_w = w; g_pop_h = h;
    g_pop_rows = vis;
    for (int i = 0; i < vis; i++) g_pop_item[i] = rows[g_pop_scroll + i];

    /* soft shadow, then translucent Voyager navy panel - same family as
     * menu bar + dock */
    fb_blend_rect(x + 5, y + 5, w, h, rgba(0, 0, 0, 80));
    fb_blend_rect(x, y, w, h, rgba(44, 13, 48, 225));
    fb_draw_rect(x, y, w, h, rgb(182, 96, 200));
    fb_blend_rect(x, y, w, 2, rgba(233, 130, 255, 130));
    fb_text(x + 10, y + 5, a->name, rgb(249, 225, 255), 1);
    for (int i = 0; i < vis; i++) {
        Window *win = &g_win[rows[g_pop_scroll + i]];
        int ry = y + 28 + i * 24;
        bool hov = (g_mx >= x + 4 && g_mx < x + w - 4 &&
                    g_my >= ry && g_my < ry + 22);
        if (hov) fb_blend_rect(x + 4, ry, w - 8, 22, rgba(150, 70, 170, 150));
        /* color-chip state indicator: cyan = live window, slate = minimized */
        fb_fill_rect(x + 10, ry + 7, 8, 8,
                     win->minimized ? rgb(151, 105, 160) : rgb(164, 96, 255));
        char t[64];
        snprintf(t, sizeof(t), "%s #%d",
                 a->title ? a->title : a->name, g_pop_scroll + i + 1);
        fb_text_clip(x + 24, ry + 7, t, rgb(249, 232, 255), 1,
                     x + 8, ry, w - 34, 20);
    }
    /* scroll hint when the list is clipped */
    if (nr > vis) {
        char st[24];
        snprintf(st, sizeof(st), "%d-%d of %d", g_pop_scroll + 1,
                 g_pop_scroll + vis, nr);
        int swid = fb_text_width(st, 1);
        fb_text(x + w - swid - 10, y + h - 18, st, rgb(194, 140, 205), 1);
    }
}

/* ---------- desktop context menu (right-click) ---------- */

/* defined further below with the desktop icon layout code */
static int g_desk_isz = DESK_ICON_SZ;  /* auto-fitted icon size this frame */
static int desk_icon_count(void);
static int desk_vfs_count(void);
static const VfsEntry *desk_vfs_at(int j);
static int desk_icon_count(void);
static int desk_icon_info(int i, const char **icon, const char **label,
                        const VfsEntry **ve);
static void desk_icon_rect(int i, int *x, int *y);
static void desk_icon_rect(int i, int *x, int *y);
static int desk_icon_info(int i, const char **icon, const char **label,
                          const VfsEntry **ve);
static uint64_t g_desk_sel = 0;            /* bitmask: bit i = icon i selected */

static int g_ctx_open, g_ctx_x, g_ctx_y, g_ctx_w, g_ctx_h;
static int g_ctx_click_x, g_ctx_click_y;   /* raw right-click point (menu
                                            * pos gets clamped for drawing) */
static int g_ctx_icon = -1;    /* desktop icon idx for icon-menu, -1 = bare */
static int g_ctx_kind = 0;     /* 0 = bare desktop, 1 = VFS icon, 2 = app icon */
static int g_ctx_nitems = 2;   /* item count for the current menu */
static const char **g_ctx_app_items; /* "Add to Dock" / "Remove from Dock" */

/* transient "This action is denied!" banner for protected system items */
static char     g_deny[64];
static uint64_t g_deny_t;
static void gui_deny(void) {
    gui_notify("This action is denied!");
    gui_alert_beep();
}
void gui_notify(const char *msg) {
    snprintf(g_deny, sizeof(g_deny), "%s", msg ? msg : "");
    g_deny_t = gui_ticks();
}
static void draw_deny(void) {
    if (!g_deny[0]) return;
    if (gui_ticks() - g_deny_t > 2500) { g_deny[0] = 0; return; }
    uint32_t sw = fb_width(), sh = fb_height();
    int mw = fb_text_width(g_deny, 1) + 28;
    int mx = ((int)sw - mw) / 2, my = (int)sh - DOCK_H - 46;
    fb_blend_rect(mx + 3, my + 3, mw, 30, rgba(0, 0, 0, 90));
    fb_blend_rounded(mx, my, mw, 30, 6, rgba(44, 13, 48, 235));
    fb_draw_rect(mx, my, mw, 30, rgb(182, 96, 200));
    fb_text(mx + 14, my + 10, g_deny, rgb(255, 210, 210), 1);
}

static void rename_open(const char *oldname);
static void rename_open_in(const char *dir, const char *oldname);
static void place_new_icon_at_mouse(const char *name);
static void place_icon_at_point(const char *name, int px, int py);

/* hit-test desktop icons (apps + VFS items); -1 = none.
 * exclude >= 0 skips that index (dragged icon sits under the cursor). */
static int desk_icon_hit_ex(int mx, int my, int exclude) {
    int n = desk_icon_count();
    for (int i = 0; i < n; i++) {
        if (i == exclude) continue;
        int ix, iy; desk_icon_rect(i, &ix, &iy);
        int isz = g_desk_isz;
        if (mx >= ix - 8 && mx < ix + isz + 8 &&
            my >= iy - 8 && my < iy + isz + 30)
            return i;
    }
    return -1;
}
static int desk_icon_hit(int mx, int my) { return desk_icon_hit_ex(mx, my, -1); }

/* drag-drop target: icon hitboxes overlap vertically, so first-match is
 * unreliable - pick the VFS folder nearest the cursor instead (only
 * folders accept drops anyway, like Windows) */
static int desk_drop_target(int mx, int my, int exclude) {
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < desk_icon_count(); i++) {
        if (i == exclude) continue;
        int ix, iy; desk_icon_rect(i, &ix, &iy);
        int isz = g_desk_isz;
        if (mx < ix - 8 || mx >= ix + isz + 8 ||
            my < iy - 8 || my >= iy + isz + 30) continue;
        const char *icon, *label; const VfsEntry *ve;
        if (desk_icon_info(i, &icon, &label, &ve) <= 0 || !ve ||
            ve->type != VFS_DIR) continue;
        int dx = mx - (ix + isz / 2), dy = my - (iy + isz / 2);
        int d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* from Finder.cpp: is a root folder currently open in any Files window? */
int finder_folder_in_use(const char *name);

/* right-click "Close All Windows": spin every window of the app out,
 * same animated path as the caption X (unsaved-change apps still get
 * their save dialog via confirm_close inside close_window_idx) */
static void close_all_app_windows(App *a) {
    if (!a) return;
    for (int i = g_win_count - 1; i >= 0; i--)
        if (g_win[i].app == a && g_win[i].visible && !g_win[i].closing)
            close_window_idx(i);
}

static void ctx_create(int type) {
    /* app icon menu: [Close All Windows] [Add/Remove to Dock] */
    if (g_ctx_kind == 2 && g_ctx_icon >= 0 && g_ctx_icon < g_app_count) {
        App *a = g_apps[g_ctx_icon];
        if (type == 0) {
            close_all_app_windows(a);
        } else {
            int de = dock_find_app(g_ctx_icon);
            if (de >= 0) dock_remove(de);
            else gui_dock_add_app(a->name);
        }
        g_ctx_icon = -1;
        return;
    }
    /* icon-menu mode: 0 = Rename, 1 = Delete, 2 = Add to Dock (VFS item) */
    if (g_ctx_icon >= 0) {
        const char *icon, *label; const VfsEntry *ve;
        if (desk_icon_info(g_ctx_icon, &icon, &label, &ve) > 0 && ve) {
            if (type == 2) {
                if (!ve->system) gui_dock_add_vfs("", ve->name);
                else gui_deny();
            } else if (ve->system) gui_deny();   /* protected: denied */
            else if (type == 0) rename_open_in(NULL, ve->name);   /* desktop = root */
            else if (ve->type == VFS_DIR && finder_folder_in_use(ve->name))
                gui_notify("Directory is still in use!");
            else { vfs_delete(ve->name); g_desk_sel = 0; }
        }
        g_ctx_icon = -1;
        return;
    }
    static const char *bases[] = { "New Folder", "New File.txt" };
    char name[40];
    for (int n = 1; n < 200; n++) {
        if (n == 1) snprintf(name, sizeof(name), "%s", bases[type]);
        else snprintf(name, sizeof(name), "%s %d", bases[type], n);
        if (vfs_find(name) < 0 &&
            vfs_create(name, type == 0 ? VFS_DIR : VFS_FILE) == 0) {
            rename_open(name);   /* "Pick a name" dialog */
            place_new_icon_at_mouse(name);
            break;
        }
    }
}

static void draw_context_menu(void) {
    if (!g_ctx_open) return;
    static const char *desk_items[] = { "New Folder", "New Text File" };
    static const char *vfs_items[] = { "Rename", "Delete", "Add to Dock" };
    static const char *app_add[]  = { "Close All Windows", "Add to Dock" };
    static const char *app_rm[]   = { "Close All Windows", "Remove from Dock" };
    const char **items;
    int n;
    if (g_ctx_kind == 2) {
        items = g_ctx_app_items ? g_ctx_app_items : app_add;
        n = 2;
    } else if (g_ctx_kind == 1) { items = vfs_items; n = g_ctx_nitems; }
    else { items = desk_items; n = 2; }
    int w = 150, h = 30 + n * 24;
    int x = g_ctx_x, y = g_ctx_y;
    uint32_t sw = fb_width(), sh = fb_height();
    if (x + w > (int)sw - 4) x = (int)sw - w - 4;
    if (y + h > (int)sh - DOCK_H - 12) y = (int)sh - DOCK_H - 12 - h;
    if (x < 4) x = 4;
    if (y < MENU_H + 4) y = MENU_H + 4;
    g_ctx_x = x; g_ctx_y = y; g_ctx_w = w; g_ctx_h = h;
    fb_blend_rect(x + 4, y + 4, w, h, rgba(0, 0, 0, 80));
    fb_blend_rect(x, y, w, h, rgba(44, 13, 48, 230));
    fb_draw_rect(x, y, w, h, rgb(182, 96, 200));
    fb_blend_rect(x, y, w, 2, rgba(233, 130, 255, 130));
    for (int i = 0; i < n; i++) {
        int ry = y + 26 + i * 24;
        bool hov = (g_mx >= x + 4 && g_mx < x + w - 4 &&
                    g_my >= ry && g_my < ry + 22);
        if (hov) fb_blend_rect(x + 4, ry, w - 8, 22, rgba(150, 70, 170, 150));
        fb_text_clip(x + 12, ry + 7, items[i], rgb(249, 232, 255), 1,
                     x + 6, ry, w - 12, 20);
    }
}

/* ---------- rename dialog ("Pick a name") ---------- */

static int  g_rename_active;
static char g_rename_old[VFS_NAME_LEN + 1];
static char g_rename_dir[VFS_NAME_LEN + 1];  /* "" = root scope */
static char g_rename_buf[40];
static int  g_rename_len;
static int  g_rename_cur;   /* caret position (char index) */
static int  g_rename_ox;    /* horizontal pixel scroll of the field */

static void rename_open_in(const char *dir, const char *oldname) {
    if (dir) {
        strncpy(g_rename_dir, dir, sizeof(g_rename_dir) - 1);
        g_rename_dir[sizeof(g_rename_dir) - 1] = 0;
    } else g_rename_dir[0] = 0;
    strncpy(g_rename_old, oldname, sizeof(g_rename_old) - 1);
    g_rename_old[sizeof(g_rename_old) - 1] = 0;
    strncpy(g_rename_buf, oldname, sizeof(g_rename_buf) - 1);
    g_rename_buf[sizeof(g_rename_buf) - 1] = 0;
    g_rename_len = (int)strlen(g_rename_buf);
    g_rename_cur = g_rename_len;
    g_rename_ox = 0;
    g_rename_active = 1;
}
static void rename_open(const char *oldname) { rename_open_in(NULL, oldname); }
/* public: apps (e.g. Files) request the "Pick a name" rename dialog */
void gui_request_rename(const char *dir, const char *name) {
    rename_open_in(dir, name);
}

/* ---------- "Do you want to save?" dialog ---------- */

/* button rects for the 3 actions, in dialog coords */
#define SAVE_DLG_W 320
#define SAVE_DLG_H 128
static void save_dlg_rect(int *dx, int *dy) {
    Window *w = &g_win[g_save_win];
    *dx = w->x + w->w / 2 - SAVE_DLG_W / 2;
    *dy = w->y + w->h / 2 - SAVE_DLG_H / 2;
    if (*dy < MENU_H + 8) *dy = MENU_H + 8;
}

static void draw_save_dialog(void) {
    if (g_save_win < 0) return;
    if (!(g_mbuttons & 1)) g_save_prs = -1;   /* released: clear press */
    if (g_save_win >= g_win_count) { g_save_win = -1; return; }
    int dx, dy; save_dlg_rect(&dx, &dy);
    fb_blend_rect(dx + 5, dy + 5, SAVE_DLG_W, SAVE_DLG_H, rgba(0, 0, 0, 100));
    fb_blend_rect(dx, dy, SAVE_DLG_W, SAVE_DLG_H, rgba(44, 13, 48, 245));
    fb_draw_rect(dx, dy, SAVE_DLG_W, SAVE_DLG_H, rgb(202, 110, 220));
    fb_blend_rect(dx, dy, SAVE_DLG_W, 2, rgba(233, 130, 255, 130));
    fb_text(dx + 16, dy + 12, "Do you want to save?", rgb(249, 225, 255), 1);
    Window *w = &g_win[g_save_win];
    fb_text(dx + 16, dy + 32, w->app ? w->app->title : "", rgb(211, 160, 220), 1);
    /* buttons: Save | Don't Save | Cancel */
    static const char *labels[] = { "Save", "Don't Save", "Cancel" };
    static const int widths[] = { 76, 104, 76 };
    int bx = dx + 16, by = dy + SAVE_DLG_H - 44;
    for (int i = 0; i < 3; i++) {
        int bw = widths[i];
        bool hov = (g_mx >= bx && g_mx < bx + bw && g_my >= by && g_my < by + 28);
        bool prs = (g_save_prs == i);
        uint32_t bg = prs ? rgba(97, 40, 110, 240) :
                      hov ? rgba(150, 70, 170, 220) : rgba(132, 60, 150, 170);
        fb_blend_rect(bx, by, bw, 28, bg);
        fb_draw_rect(bx, by, bw, 28, prs ? rgb(152, 80, 170) : rgb(202, 110, 220));
        int tw = fb_text_width(labels[i], 1);
        fb_text(bx + (bw - tw) / 2, by + 9, labels[i], rgb(250, 235, 255), 1);
        bx += bw + 10;
    }
}

/* returns 1 if the click was consumed by the dialog */
static int save_dialog_click(int mx, int my) {
    if (g_save_win < 0) return 0;
    if (g_save_win >= g_win_count) { g_save_win = -1; return 0; }
    int dx, dy; save_dlg_rect(&dx, &dy);
    bool inside = (mx >= dx && mx < dx + SAVE_DLG_W &&
                   my >= dy && my < dy + SAVE_DLG_H);
    if (!inside) { g_save_win = -1; g_save_prs = -1; return 1; }   /* click-away = cancel */
    static const int widths[] = { 76, 104, 76 };
    int bx = dx + 16, by = dy + SAVE_DLG_H - 44;
    g_save_prs = -1;
    for (int i = 0; i < 3; i++) {
        if (mx >= bx && mx < bx + widths[i] && my >= by && my < by + 28) {
            g_save_prs = i;
            Window *w = &g_win[g_save_win];
            int win = g_save_win;
            g_save_win = -1;
            if (i == 0) {                        /* Save */
                if (w->app && w->app->save_now && w->ctx)
                    w->app->save_now(w->ctx);
                /* close only if nothing is still pending: named docs save
                 * in place and close now, but untitled docs just opened the
                 * save-as picker - closing here would re-trigger the
                 * "Do you want to save?" veto and loop the dialog */
                if (!w->app || !w->app->confirm_close || !w->ctx ||
                    !w->app->confirm_close(w->ctx))
                    close_window_idx(win);
            } else if (i == 1) {                 /* Don't Save */
                g_force_close = 1;
                close_window_idx(win);
            }                                   /* Cancel: keep window */
            return 1;
        }
        bx += widths[i] + 10;
    }
    return 1;   /* clicked dialog but not a button: swallow */
}

/* ---------- system alert dialog ----------
 * Modal popup with a big status icon: red circle-X (error), amber
 * triangle-! (warning), blue circle-i (info). Slides down from the top
 * on open, dismisses on OK click / Enter / Esc / Space. */
#define ALERT_ERR  0
#define ALERT_WARN 1
#define ALERT_INFO 2
static char g_alert_lines[40][100];
static struct {
    int active, kind;
    char title[64];
    char msg[256];
    uint64_t t0;
    int ok_x, ok_y, ok_w, ok_h;
    int dx, dy, dw, dh;         /* dialog rect (set each draw) */
    int bx0, by0;               /* centered base pos (pre-drag) */
    int ox, oy;                 /* drag offset from base pos */
    int dragging, gx0, gy0;     /* titlebar drag state + grab point */
    uint32_t *snap;             /* rendered dialog for spin anims */
    int snap_w, snap_h;
    int anim;                   /* 0 none, 1 spin-in, 2 spin-out */
    uint64_t anim_t;
} g_alert;

static void alert_show(int kind, const char *title, const char *msg) {
    if (g_alert.snap) { kfree(g_alert.snap); g_alert.snap = NULL; }
    g_alert.active = 1;
    g_alert.kind = kind;
    strncpy(g_alert.title, title ? title : "Voyager OS", sizeof(g_alert.title) - 1);
    g_alert.title[sizeof(g_alert.title) - 1] = 0;
    strncpy(g_alert.msg, msg ? msg : "", sizeof(g_alert.msg) - 1);
    g_alert.msg[sizeof(g_alert.msg) - 1] = 0;
    g_alert.t0 = pit_ticks();
    g_alert.ox = g_alert.oy = 0;
    g_alert.dragging = 0;
    g_alert.anim = 1;               /* spin in like every app window */
    g_alert.anim_t = pit_ticks();
}

/* close = SPIN-OUT like windows: the dialog shrinks+rotates away over
 * ~300ms, THEN active clears. Snap is captured on the first draw if the
 * open spin-in never produced one */
static void alert_dismiss(void) {
    if (!g_alert.active || g_alert.anim == 2) return;
    g_alert.anim = 2;
    g_alert.anim_t = pit_ticks();
    g_alert.dragging = 0;
    gui_set_cursor(-1);
}

/* public wrapper: kind 0=error 1=warning 2=info */
void gui_alert(int kind, const char *title, const char *msg) {
    alert_show(kind, title, msg);
}

/* paint the dialog body at (x,y) size w x h. live=0 renders a neutral
 * snapshot frame (no hover/press/cursor state) for the spin blits */
static void alert_paint(int x, int y, int w, int h, int nlines, int live) {
    fb_blend_rect(x + 6, y + 8, w, h, rgba(0, 0, 0, 110));
    fb_blend_rect(x, y, w, h, rgba(250, 250, 253, 250));
    fb_draw_rect(x, y, w, h, rgb(150, 150, 165));

    /* ---- titlebar: app-window band, square like every other dialog ---- */
    fb_fill_rect(x, y, w, 30, rgb(233, 236, 244));
    fb_fill_rect(x, y + 29, w, 1, rgb(204, 186, 208));
    fb_text(x + 14, y + 11, g_alert.title, rgb(77, 52, 80), 1);

    /* ---- status badge: ROUND disc + the OS's own font glyphs ---- */
    int bs = 48, icx = x + 14 + bs / 2, icy = y + 30 + (h - 30) / 2 + 4;
    uint32_t face, ink;
    const char *glyph;
    int gs = 4;                                 /* 8x8 font -> 32px glyph */
    if (g_alert.kind == ALERT_ERR) {
        face = rgb(214, 40, 40);  ink = rgb(255, 255, 255); glyph = "X";
    } else if (g_alert.kind == ALERT_WARN) {
        face = rgb(240, 178, 40); ink = rgb(70, 45, 5);     glyph = "!";
    } else {
        face = rgb(173, 60, 230); ink = rgb(255, 255, 255); glyph = "i";
    }
    fb_fill_rounded(icx - bs / 2, icy - bs / 2, bs, bs, bs / 2, face);
    fb_blend_rounded(icx - bs / 2 - 3, icy - bs / 2 - 3, bs + 6, bs + 6,
                     bs / 2 + 3, rgba(0, 0, 0, 60));   /* soft drop shadow */
    fb_fill_rounded(icx - bs / 2, icy - bs / 2, bs, bs, bs / 2, face);
    /* center the single glyph: 8*gs px wide/tall */
    fb_text(icx - 4 * gs, icy - 4 * gs, glyph, ink, gs);

    /* ---- text: message lines under the titlebar ---- */
    int tx = x + 14 + bs + 16;
    int cy2 = y + 30 + 20;
    for (int i = 0; i < nlines; i++) {
        fb_text(tx, cy2, g_alert_lines[i], rgb(87, 60, 90), 1);
        cy2 += 16;
    }
    /* OK button - square push-button, same style as window/save dialogs */
    int bw = 84, bh = 26;
    int bx = x + w - bw - 18, by = y + h - bh - 14;
    bool hov = live && g_mx >= bx && g_mx < bx + bw &&
               g_my >= by && g_my < by + bh;
    bool dn = hov && (g_mbuttons & 1);
    if (live) gui_set_cursor(hov ? CUR_HAND : CUR_ARROW);
    fb_fill_rect(bx, by, bw, bh,
                 dn ? rgb(128, 38, 150) : (hov ? rgb(183, 70, 220) : rgb(161, 52, 190)));
    fb_draw_rect(bx, by, bw, bh, rgb(117, 30, 140));
    int tw = fb_text_width("OK", 1);
    fb_text(bx + (bw - tw) / 2, by + 9, "OK", rgb(255, 255, 255), 1);
    /* hint sits at the LEFT edge of the button row - far from the button
     * so it can never clip through it */
    fb_text(x + 18, by + 9, "click OK or press Enter", rgb(158, 140, 160), 1);
}

static void draw_alert(void) {
    if (!g_alert.active) return;
    uint32_t sw = fb_width(), sh = fb_height();
    int w = 400;
    /* wrap the message to the text column's real pixel width (~286px):
     * take as many chars per line as actually fit, break at the last
     * space when possible, repeat until the whole message is consumed */
    int nlines = 0;
    {
        const char *p = g_alert.msg;
        while (*p && nlines < 40) {
            int n = 0, wpx = 0;
            while (p[n] && p[n] != '\n' && n < 96) {
                char tc[2] = { p[n], 0 };
                int cw = fb_text_width(tc, 1);
                if (wpx + cw > 282 && n > 0) break;
                wpx += cw; n++;
            }
            /* break at the last space inside the window if there is one */
            if (p[n] && p[n] != '\n' && n > 10) {
                int sp = n - 1;
                while (sp > 4 && p[sp] != ' ') sp--;
                if (p[sp] == ' ') n = sp;
            }
            if (n <= 0 && p[0] && p[0] != '\n') { g_alert_lines[nlines][0] = p[0]; g_alert_lines[nlines][1] = 0; n = 1; }
            else { memcpy(g_alert_lines[nlines], p, (size_t)n); g_alert_lines[nlines][n] = 0; }
            nlines++;
            p += n;
            if (*p == ' ') p++;          /* swallow the break space */
            else if (*p == '\n') p++;
            else if (n == 0 && *p) p++;  /* safety: always advance */
        }
    }
    if (nlines < 1) nlines = 1;
    const int TB = 30;                       /* titlebar band = app window */
    int h = TB + 26 + nlines * 16 + 56;
    if (h < 190) h = 190;
    if (h > (int)sh - 80) h = (int)sh - 80;
    int bx0 = ((int)sw - w) / 2;
    int x = bx0 + g_alert.ox;
    int y = (int)((sh - h) / 2) + 34 + g_alert.oy;
    /* keep the whole dialog on screen while dragging */
    if (x < 4) x = 4; if (x + w > (int)sw - 4) x = (int)sw - w - 4;
    if (y < MENU_H + 4) y = MENU_H + 4; if (y + h > (int)sh - 4) y = (int)sh - h - 4;
    g_alert.dx = x; g_alert.dy = y; g_alert.dw = w; g_alert.dh = h;
    g_alert.bx0 = bx0; g_alert.by0 = (int)((sh - h) / 2) + 34;
    /* hit rect for the OK button (paint computes the same geometry) */
    g_alert.ok_x = x + w - 84 - 18; g_alert.ok_y = y + h - 26 - 14;
    g_alert.ok_w = 84; g_alert.ok_h = 26;

    /* ---- spin animations, identical curves to app windows ---- */
    if (g_alert.anim) {
        /* lazily render the snapshot: padded buffer, dialog centered */
        if (!g_alert.snap) {
            int pw = w + 16, ph = h + 16;
            uint32_t *buf = (uint32_t *)kmalloc((size_t)pw * ph * 4);
            if (buf) {
                memset(buf, 0, (size_t)pw * ph * 4);
                uint32_t *oa; uint32_t op;
                fb_get_target(&oa, &op);
                fb_set_target(buf, (uint32_t)pw * 4);
                fb_set_clip((uint32_t)pw, (uint32_t)ph);
                alert_paint(8, 8, w, h, nlines, 0);
                fb_set_target(oa, op);
                fb_set_clip((uint32_t)sw, (uint32_t)sh);
                g_alert.snap = buf;
                g_alert.snap_w = pw; g_alert.snap_h = ph;
            }
        }
        uint64_t el = pit_ticks() - g_alert.anim_t;
        if (g_alert.anim == 1) {
            if (el >= 280) {          /* spin-in done -> live paint */
                g_alert.anim = 0;
                if (g_alert.snap) { kfree(g_alert.snap); g_alert.snap = NULL; }
            } else if (g_alert.snap) {
                /* -35deg -> 0, 55% -> 100%, ease-out (mirror of spin-out) */
                int t = (int)(el * 100 / 280);
                int e = 100 - t;
                int pct = 100 - (45 * e * e) / 10000;
                int ee = 100 - (e * e) / 100;
                int ang = -((100 - ee) * 35) / 100;
                int nw = g_alert.snap_w * pct / 100;
                int nh = g_alert.snap_h * pct / 100;
                if (nw < 8) nw = 8; if (nh < 8) nh = 8;
                fb_blit_spin(x + w / 2, y + h / 2, nw, nh, ang,
                             g_alert.snap, g_alert.snap_w, g_alert.snap_h,
                             g_alert.snap_w, g_alert.snap_h, 0, 0);
                return;
            }
        } else {                       /* anim == 2: spin-out */
            if (el >= 300) {           /* gone for good */
                g_alert.active = 0;
                g_alert.anim = 0;
                if (g_alert.snap) { kfree(g_alert.snap); g_alert.snap = NULL; }
                return;
            }
            if (g_alert.snap) {
                /* 0 -> +35deg, 100% -> 0%, ease-in (same as windows) */
                int t = (int)(el * 100 / 300);
                int e = (t * t) / 100;
                int pct = 100 - e;
                int ang = (e * 35) / 100;
                int nw = g_alert.snap_w * pct / 100;
                int nh = g_alert.snap_h * pct / 100;
                if (nw < 4) nw = 4; if (nh < 4) nh = 4;
                fb_blit_spin(x + w / 2, y + h / 2, nw, nh, ang,
                             g_alert.snap, g_alert.snap_w, g_alert.snap_h,
                             g_alert.snap_w, g_alert.snap_h, 0, 0);
                return;
            }
            /* no snapshot available: just vanish */
            g_alert.active = 0; g_alert.anim = 0; return;
        }
    }
    alert_paint(x, y, w, h, nlines, 1);
}

/* returns 1 if the click was swallowed by the alert */
static int alert_click(int mx, int my) {
    if (!g_alert.active) return 0;
    if (mx >= g_alert.ok_x && mx < g_alert.ok_x + g_alert.ok_w &&
        my >= g_alert.ok_y && my < g_alert.ok_y + g_alert.ok_h) {
        alert_dismiss();
        return 1;
    }
    /* titlebar drag: press anywhere in the band and pull the dialog
     * around like a normal window (not while it's spinning) */
    if (g_alert.anim == 0 &&
        mx >= g_alert.dx && mx < g_alert.dx + g_alert.dw &&
        my >= g_alert.dy && my < g_alert.dy + 30) {
        g_alert.dragging = 1;
        g_alert.gx0 = mx - g_alert.dx;
        g_alert.gy0 = my - g_alert.dy;
    }
    return 1;   /* modal: swallow every click while shown */
}

/* returns 1 if the key was consumed by the alert */
static int alert_key(int c) {
    if (!g_alert.active) return 0;
    if (c == '\r' || c == '\n' || c == ' ' || c == 27) alert_dismiss();
    return 1;
}

static void draw_rename_dialog(void) {
    if (!g_rename_active) return;
    uint32_t sw = fb_width(), sh = fb_height();
    int w = 280, h = 92;
    int x = ((int)sw - w) / 2, y = ((int)sh - h) / 2;
    fb_blend_rect(x + 5, y + 5, w, h, rgba(0, 0, 0, 90));
    fb_blend_rect(x, y, w, h, rgba(44, 13, 48, 240));
    fb_draw_rect(x, y, w, h, rgb(202, 110, 220));
    fb_text(x + 14, y + 10, "Pick a name", rgb(249, 225, 255), 1);
    /* text field */
    fb_fill_rect(x + 14, y + 32, w - 28, 24, rgb(238, 240, 248));
    fb_draw_rect(x + 14, y + 32, w - 28, 24, rgb(161, 120, 170));
    /* keep the caret inside the visible field window */
    int fx = x + 20, fw = w - 36;
    int cx = g_rename_cur * 9;
    if (cx < g_rename_ox) g_rename_ox = cx;
    if (cx + 9 > g_rename_ox + fw) g_rename_ox = cx + 9 - fw;
    if (g_rename_ox < 0) g_rename_ox = 0;
    fb_text_clip(fx - g_rename_ox, y + 39, g_rename_buf,
                 rgb(47, 20, 50), 1, x + 14, y + 32, w - 28, 24);
    /* I-beam over the text field, arrow elsewhere while the dialog is up */
    bool inf = g_mx >= x + 14 && g_mx < x + w - 14 &&
               g_my >= y + 32 && g_my < y + 56;
    gui_set_cursor(inf ? CUR_IBEAM : CUR_ARROW);
    /* caret at the cursor position */
    if ((pit_ticks() / 500) & 1)
        fb_fill_rect(fx - g_rename_ox + cx, y + 36, 2, 14, rgb(47, 20, 50));
    fb_text(x + 14, y + 66, "Enter = save", rgb(201, 150, 210), 1);
    fb_text(x + 14, y + 80, "Esc = keep name", rgb(201, 150, 210), 1);
}

/* returns 1 if the keypress was consumed by the dialog */
static int rename_key(int c) {
    if (!g_rename_active) return 0;
    if (c == 27) { g_rename_active = 0; gui_set_cursor(-1); return 1; }              /* Esc */
    if (c == '\r' || c == '\n') {                                 /* Enter */
        char nb[40];
        strncpy(nb, g_rename_buf, sizeof(nb) - 1); nb[sizeof(nb)-1] = 0;
        /* trim trailing spaces */
        int l = (int)strlen(nb);
        while (l > 0 && nb[l-1] == ' ') nb[--l] = 0;
        if (l > 0 && strcmp(nb, g_rename_old))
            vfs_rename_in(g_rename_dir[0] ? g_rename_dir : NULL,
                          g_rename_old, nb);
        g_rename_active = 0;
        gui_set_cursor(-1);
        return 1;
    }
    /* arrow keys + editing specials (KEY_* >= 0x80 arrive raw) */
    if (c == KEY_LEFT)  { if (g_rename_cur > 0) g_rename_cur--; return 1; }
    if (c == KEY_RIGHT) { if (g_rename_cur < g_rename_len) g_rename_cur++; return 1; }
    if (c == KEY_HOME || c == KEY_UP)   { g_rename_cur = 0; return 1; }
    if (c == KEY_END  || c == KEY_DOWN) { g_rename_cur = g_rename_len; return 1; }
    if (c == KEY_DELETE) {                                     /* Del key */
        if (g_rename_cur < g_rename_len) {
            memmove(g_rename_buf + g_rename_cur, g_rename_buf + g_rename_cur + 1,
                    (size_t)(g_rename_len - g_rename_cur));
            g_rename_len--;
            g_rename_buf[g_rename_len] = 0;
        }
        return 1;
    }
    if (c == 8) {                                                 /* backspace */
        if (g_rename_cur > 0) {
            memmove(g_rename_buf + g_rename_cur - 1,
                    g_rename_buf + g_rename_cur,
                    (size_t)(g_rename_len - g_rename_cur) + 1);
            g_rename_cur--;
            g_rename_len--;
        }
        return 1;
    }
    if (c >= 32 && c < 127 && g_rename_len < 38) {                /* printable */
        memmove(g_rename_buf + g_rename_cur + 1, g_rename_buf + g_rename_cur,
                (size_t)(g_rename_len - g_rename_cur) + 1);
        g_rename_buf[g_rename_cur++] = (char)c;
        g_rename_len++;
        return 1;
    }
    return 1;   /* swallow everything else while dialog is up */
}

/* ---------- desktop icons ---------- */

static int g_desk_scroll;   /* pixel offset: wheel-scrolls the icon column */
static int g_desk_cols = 1;
static int16_t g_icon_dx[64], g_icon_dy[64];  /* drag offsets per app icon */

/* desktop items = registered apps followed by VFS files/folders */
/* desktop shows user files only - protected system entries live inside
 * the System HD volume view (Files app), not on the desktop */
static int desk_vfs_count(void) {
    int n = 0, vc = vfs_count();
    for (int i = 0; i < vc; i++) {
        const VfsEntry *e = vfs_entry(i);
        if (e && !e->system) n++;
    }
    return n;
}
static const VfsEntry *desk_vfs_at(int j) {
    int vc = vfs_count();
    for (int i = 0; i < vc; i++) {
        const VfsEntry *e = vfs_entry(i);
        if (e && !e->system && j-- == 0) return e;
    }
    return NULL;
}
static int desk_icon_count(void) { return g_app_count + desk_vfs_count(); }

/* icon for a VFS file by extension: text docs get Quill's icon, anything
 * else gets the big "?" unknown-file icon */
const char *gui_file_icon(const char *name) {
    int l = (int)strlen(name);
    if (l > 4 && !strcmp(name + l - 4, ".txt")) return "quill";
    if (gui_is_audio(name)) return "voyage player";
    if (l > 4 && !strcmp(name + l - 4, ".lnk")) {
        /* shortcut: "Calc.lnk" -> show the target app's own icon */
        char base[40];
        int n = l - 4 < 39 ? l - 4 : 39;
        memcpy(base, name, n); base[n] = 0;
        App *a = gui_find_app(base);
        if (a) return a->name;
    }
    return "unknown";
}

/* resolve desktop index -> icon asset name + label; returns 0 for app,
 * 1 for vfs entry (out params filled) */
static int desk_icon_info(int i, const char **icon, const char **label,
                          const VfsEntry **ve) {
    if (i < g_app_count) {
        *icon = g_apps[i]->name;
        *label = g_apps[i]->name;
        *ve = NULL;
        return 0;
    }
    const VfsEntry *e = desk_vfs_at(i - g_app_count);
    if (!e) return -1;
    *icon = (e->type == VFS_DIR) ? "files" : gui_file_icon(e->name);
    *label = e->name;
    *ve = e;
    return 1;
}
/* stable position key: apps keep their index, VFS items keep their slot
 * index - so adding/removing icons NEVER shifts the others (pinned like
 * real desktop icons). Keys index g_icon_dx/dy directly. */
static int desk_key(int i) {
    if (i < g_app_count) return i;
    const VfsEntry *e = desk_vfs_at(i - g_app_count);
    if (!e) return i;
    int s = vfs_slot_root(e->name);
    return (s < 0) ? i : g_app_count + s;
}

static void desk_icon_rect(int i, int *x, int *y) {
    uint32_t sw = fb_width(), sh = fb_height();
    int top = MENU_H + 10;
    int bot = (int)sh - DOCK_H - 16 - 8;
    int avail = bot - top;
    /* icons keep their original size - never shrink or rescale */
    int isz = DESK_ICON_SZ;
    int stride = isz + 18;
    int key = desk_key(i);
    /* fixed grid derived from the stable key: columns fill top-down from
     * the right edge; a new slot takes the next free cell without moving
     * any existing icon */
    int rows = avail / stride;
    if (rows < 1) rows = 1;
    int col = key / rows, row = key % rows;
    g_desk_isz = isz;
    g_desk_cols = col + 1;
    g_desk_scroll = 0;
    int xright = (int)sw - isz - 24;
    int ox = xright - col * stride + g_icon_dx[key];
    int oy = top + row * stride + g_icon_dy[key];
    /* clamp dragged icons inside the desktop area (below menu, above dock) */
    if (ox < 8) ox = 8;
    if (ox + isz > (int)sw - 8) ox = (int)sw - isz - 8;
    if (oy < MENU_H + 4) oy = MENU_H + 4;
    if (oy + isz > (int)sh - DOCK_H - 12) oy = (int)sh - DOCK_H - 12 - isz;
    *x = ox;
    *y = oy;
}

/* spawn a freshly created VFS icon centered on the right-click point
 * instead of the default grid slot */
static void place_new_icon_at_mouse(const char *name) {
    place_icon_at_point(name, g_ctx_click_x, g_ctx_click_y);
}

/* pin a VFS icon's center on an explicit screen point (used by context-
 * menu creation AND dock drag-out shortcuts) */
static void place_icon_at_point(const char *name, int px, int py) {
    int j = -1;
    for (int i = 0; i < desk_vfs_count(); i++) {
        const VfsEntry *e = desk_vfs_at(i);
        if (e && !strcmp(e->name, name)) { j = i; break; }
    }
    if (j < 0) return;
    int idx = g_app_count + j, k = desk_key(idx), ix, iy;
    /* clear any stale offset for this slot FIRST, then measure the pure
     * grid cell - otherwise a recycled slot's old drag offset shifts the
     * spawn point away from the cursor */
    g_icon_dx[k] = 0; g_icon_dy[k] = 0;
    desk_icon_rect(idx, &ix, &iy);
    /* pin the icon+label block's center exactly on the point (label sits
     * ~18px under the icon, so bias y by half of it) */
    g_icon_dx[k] = (int16_t)(px - ix - g_desk_isz / 2);
    g_icon_dy[k] = (int16_t)(py - iy - (g_desk_isz + 18) / 2);
    settings_save_icons(g_icon_dx, g_icon_dy);
}

/* create/refresh a ".lnk" shortcut to `appname` inside `dir` ("" = root);
 * returns the created filename in `out` (may be NULL) */
static int make_lnk_file(const char *dir, const char *appname,
                         char *out, int cap) {
    char nm[VFS_NAME_LEN + 1];
    snprintf(nm, sizeof(nm), "%s.lnk", appname);
    if (nm[VFS_NAME_LEN]) nm[VFS_NAME_LEN] = 0;
    const char *d = (dir && dir[0]) ? dir : NULL;
    if (vfs_create_in(nm, VFS_FILE, d) != 0 && vfs_find_in(d, nm) < 0)
        return -1;
    vfs_write_in(d, nm, appname, (int)strlen(appname));
    if (out && cap > 0) {
        strncpy(out, nm, cap - 1);
        out[cap - 1] = 0;
    }
    return 0;
}

static void draw_desktop_icons(uint64_t sel) {
    for (int i = 0; i < desk_icon_count(); i++) {
        const char *icon, *label; const VfsEntry *ve;
        if (desk_icon_info(i, &icon, &label, &ve) < 0) continue;
        int ix, iy; desk_icon_rect(i, &ix, &iy);
        int isz = g_desk_isz;
        if (iy + isz < MENU_H || iy > (int)fb_height() - DOCK_H - 20) continue;
        /* .png files show a real decoded preview instead of the "?" icon */
        bool drew_png = false;
        if (ve && ve->type == VFS_FILE) {
            if (gui_is_png(ve->name)) {
                uint32_t iw = 0, ih = 0;
                const uint8_t *img = gui_vfs_thumb("", ve->name, &iw, &ih);
                if (img) {
                    fb_draw_image_scaled(ix, iy, isz, isz, img,
                                         (int)iw, (int)ih);
                    drew_png = true;
                }
            }
        }
        if (!drew_png) asset_draw(ix, iy, isz, icon);
        /* long names shrink to "name..." so they never overflow the icon */
        char fl[48];
        gui_fit_label(fl, sizeof(fl), label, isz + 44);
        int lw = fb_text_width(fl, 1);
        int lx = ix + (isz - lw) / 2;
        int ly = iy + isz + 4;
        if (sel & (1ULL << i))
            fb_blend_rect(lx - 5, ly - 2, lw + 10, 15, rgba(255, 255, 255, 210));
        fb_text(lx, ly, fl, rgb(0, 0, 0), 1);
        if (sel & (1ULL << i)) {
            /* color-tint selection: icon darkens to show it's picked */
            fb_blend_rect(ix, iy, isz, isz, rgba(37, 10, 40, 120));
            fb_blend_rect(ix, iy, isz, isz, rgba(138, 0, 255, 60));
        }
    }
}

/* ---------- window drawing ---------- */

/* draw a 1px line between two points (for titlebar glyphs) */
static void tl_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) { fb_put_pixel(x0, y0, c); return; }
    for (int i = 0; i <= steps; i++) {
        fb_put_pixel(x0 + dx * i / steps, y0 + dy * i / steps, c);
    }
}

static void draw_window(int idx) {
    Window *w = &g_win[idx];
    if (!w->visible || w->minimized) return;
    /* window close / minimize: SPIN-OUT - the window rotates away about
     * its center while shrinking to nothing over ~300ms (ease-in),
     * blitted from the snapshot so the whole window spins as one piece.
     * Closing is destroyed by the sweep; minimizing hides the window
     * (minimized + focus handoff) once the animation completes. */
    if (w->closing || w->min_out) {
        uint64_t t0 = w->closing ? w->close_t : w->min_t;
        uint64_t el = pit_ticks() - t0;
        if (el >= 300) {
            if (w->closing) return;   /* sweep removes it this frame */
            /* minimize finished: hide + hand focus to next visible */
            w->min_out = 0;
            free_window_snapshot(w);
            w->minimized = true;
            w->visible = false;
            w->focused = false;
            for (int i = g_win_count - 1; i >= 0; i--)
                if (g_win[i].visible && !g_win[i].minimized) {
                    focus_window(i); break;
                }
            return;
        }
        int t = (int)(el * 100 / 300);          /* 0..99 */
        int e = (t * t) / 100;                  /* ease-in 0..98 */
        int pct = 100 - e;                      /* 100 -> 0 */
        int ang = (e * 35) / 100;               /* 0 -> 35deg spin-out */
        int sw2 = w->snap ? w->snap_w : (w->snap_bands ? w->snap_w : w->cw0 + 8);
        int sh2 = w->snap ? w->snap_h : (w->snap_bands ? w->snap_h : w->ch0 + 8);
        int nw = sw2 * pct / 100; if (nw < 4) nw = 4;
        int nh = sh2 * pct / 100; if (nh < 4) nh = 4;
        int cx = w->cx0 + w->cw0 / 2;
        int cy = w->cy0 + w->ch0 / 2;
        if (w->snap)
            fb_blit_spin(cx, cy, nw, nh, ang, w->snap, sw2, sh2,
                         sw2, sh2, w->snap_ox, w->snap_oy);
        else if (w->snap_bands)
            fb_blit_spin_bands(cx, cy, nw, nh, ang,
                               (const uint32_t * const *)w->snap_bands,
                               w->snap_band_rows, sw2, sh2,
                               sw2, sh2, w->snap_ox, w->snap_oy);
        else {
            fb_fill_rect(cx - nw / 2, cy - nh / 2, nw, nh,
                         rgb(243, 243, 246));
            fb_draw_rect(cx - nw / 2, cy - nh / 2, nw, nh,
                         rgb(160, 160, 168));
        }
        return;
    }
    /* zoom (maximize/restore): live RESIZE animation - every frame the
     * window's real rect glides from the old frame to the new one with
     * a slight boing overshoot, and the app relayouts at each size so
     * text stays crisp (no scaled snapshot). All four corners move
     * together, exactly like dragging a corner grip - but uniform. */
    if (w->zooming) {
        uint64_t el = pit_ticks() - w->zoom_t;
        if (el >= 380) {
            w->zooming = 0;
            w->x = w->ztx; w->y = w->zty; w->w = w->ztw; w->h = w->zth;
        } else {
            int t = (int)(el * 1000 / 380);      /* 0..999 permille */
            /* ease-out-back: overshoots ~5% past target mid-flight */
            int64_t u = t - 1000;
            int64_t f = 1000 + (2700 * u * u * u) / 1000000000 +
                              (1700 * u * u) / 1000000;
            if (f < 0) f = 0;
            w->x = w->zfx + (int)(((int64_t)(w->ztx - w->zfx) * f) / 1000);
            w->y = w->zfy + (int)(((int64_t)(w->zty - w->zfy) * f) / 1000);
            w->w = w->zfw + (int)(((int64_t)(w->ztw - w->zfw) * f) / 1000);
            w->h = w->zfh + (int)(((int64_t)(w->zth - w->zfh) * f) / 1000);
            if (w->w < 8) w->w = 8; if (w->h < 8) w->h = 8;
            /* fall through: the window draws LIVE at this size every
             * frame - the app sees real resizes, not a stretched image */
        }
    }
    /* window-open: SPIN-IN - the window rotates from -35deg into place
     * while scaling 55 -> 100 about its center over ~280ms ease-out,
     * blitted from the snapshot captured at open. Mirror of spin-out. */
    if (w->open_t) {
        uint64_t el = pit_ticks() - w->open_t;
        if (el >= 280) {
            free_window_snapshot(w);
            w->open_t = 0;
        } else {
            int t = (int)(el * 100 / 280);          /* 0..99 */
            int e = 100 - t;
            int pct = 100 - (45 * e * e) / 10000;   /* 55 -> 100 ease-out */
            int ee = 100 - (e * e) / 100;           /* 0 -> 100 ease-out */
            int ang = -((100 - ee) * 35) / 100;     /* -35 -> 0deg spin-in */
            int sw2 = w->snap ? w->snap_w : (w->snap_bands ? w->snap_w : w->w + 8);
            int sh2 = w->snap ? w->snap_h : (w->snap_bands ? w->snap_h : w->h + 8);
            int nw = sw2 * pct / 100; if (nw < 8) nw = 8;
            int nh = sh2 * pct / 100; if (nh < 8) nh = 8;
            int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
            if (w->snap)
                fb_blit_spin(cx, cy, nw, nh, ang, w->snap, sw2, sh2,
                             sw2, sh2, w->snap_ox, w->snap_oy);
            else if (w->snap_bands)
                fb_blit_spin_bands(cx, cy, nw, nh, ang,
                                   (const uint32_t * const *)w->snap_bands,
                                   w->snap_band_rows, sw2, sh2,
                                   sw2, sh2, w->snap_ox, w->snap_oy);
            else {
                /* no snapshot (alloc failed): plain growing frame */
                fb_fill_rect(cx - nw / 2, cy - nh / 2, nw, nh,
                             rgb(243, 243, 246));
                fb_draw_rect(cx - nw / 2, cy - nh / 2, nw, nh,
                             rgb(160, 160, 168));
            }
            return;
        }
    }
    /* layered shadow: 3 passes fading outward for a soft premium edge */
    fb_blend_rect(w->x + 6, w->y + 6, w->w, w->h, rgba(0, 0, 0, 18));
    fb_blend_rect(w->x + 4, w->y + 4, w->w, w->h, rgba(0, 0, 0, 30));
    fb_blend_rect(w->x + 2, w->y + 2, w->w, w->h, rgba(0, 0, 0, 48));
    /* body: boxy square window, hairline border */
    fb_fill_rect(w->x, w->y, w->w, w->h, rgb(243, 243, 246));
    fb_draw_rect(w->x, w->y, w->w, w->h, rgb(160, 160, 168));
    /* titlebar band - focused gets a faint Voyager-space blue tint */
    uint32_t tb = w->focused ? rgb(244, 228, 250) : rgb(226, 227, 233);
    fb_fill_rect(w->x, w->y, w->w, TITLE_H, tb);
    if (w->focused)
        fb_blend_rect(w->x, w->y, w->w, TITLE_H, rgba(193, 90, 220, 26));
    fb_fill_rect(w->x, w->y + TITLE_H - 1, w->w, 1, rgb(198, 198, 204));
    /* caption buttons on the RIGHT: orange min, green zoom, red close.
     * Static glyphs - hover lightens, press darkens, hand cursor.
     * Non-resizable apps drop the zoom button and the min button slides
     * right so _ and X stay adjacent (no empty gap). */
    int cy = w->y + TITLE_H / 2;
    uint32_t base[3] = {rgb(255, 166, 40), rgb(52, 199, 89),  rgb(255, 95, 86)};
    uint32_t lite[3] = {rgb(255, 196, 110), rgb(110, 220, 140), rgb(255, 138, 128)};
    uint32_t dark[3] = {rgb(235, 140, 30),  rgb(40, 175, 75),   rgb(228, 72, 62)};
    uint32_t glyph = rgb(84, 42, 40);
    int nbtn = w->no_resize ? 2 : 3;
    for (int s = 0; s < nbtn; s++) {
        int pos = w->no_resize ? s + 1 : s;   /* x slot: skip zoom's spot */
        int slot = w->no_resize ? (s == 0 ? 0 : 2) : s;  /* btn type */
        int bx = w->x + w->w - 70 + pos * 24;
        int hov = (g_mx >= bx - 8 && g_mx < bx + 8 &&
                   g_my >= cy - 8 && g_my < cy + 8);
        int dn = hov && (g_mbuttons & 1);   /* pressed: darker shade */
        if (hov) gui_set_cursor(CUR_HAND);
        fb_fill_rect(bx - 8, cy - 8, 16, 16,
                     dn ? dark[slot] : (hov ? lite[slot] : base[slot]));
        fb_draw_rect(bx - 8, cy - 8, 16, 16, rgba(0, 0, 0, 60));
        uint32_t gc = hov ? rgb(60, 30, 28) : glyph;
        if (slot == 0) {          /* minimize: underscore */
            tl_line(bx - 4, cy + 3, bx + 4, cy + 3, gc);
        } else if (slot == 1) {   /* zoom: square outline (boxy "O") */
            tl_line(bx - 4, cy - 4, bx + 4, cy - 4, gc);
            tl_line(bx + 4, cy - 4, bx + 4, cy + 4, gc);
            tl_line(bx + 4, cy + 4, bx - 4, cy + 4, gc);
            tl_line(bx - 4, cy + 4, bx - 4, cy - 4, gc);
        } else {                  /* close: X */
            tl_line(bx - 4, cy - 4, bx + 4, cy + 4, gc);
            tl_line(bx - 4, cy + 4, bx + 4, cy - 4, gc);
        }
    }
    /* title: app icon + centered text (Squarebin style) */
    const char *title = w->app ? w->app->title : "?";
    int tw = fb_text_width(title, 1);
    int icon_sz = 20, gap = 6;
    int group_w = icon_sz + gap + tw;
    int gx = w->x + (w->w - group_w) / 2;
    int gy = w->y + (TITLE_H - icon_sz) / 2;
    if (w->app) asset_draw(gx, gy, icon_sz, w->app->name);
    fb_text(gx + icon_sz + gap, w->y + (TITLE_H - 8) / 2, title,
            rgb(0, 0, 0), 1);

    /* content: clip by drawing within bounds - apps must respect */
    g_cur_win = w;
    g_cur_cx = content_x(w);
    g_cur_cy = content_y(w);
    fb_fill_rect(g_cur_cx, g_cur_cy, content_w(w), content_h(w), rgb(255, 255, 255));
    if (w->app && w->app->draw) w->app->draw(w->ctx);
    g_cur_win = NULL;
    /* the app's own draw may have set a cursor - caption buttons still
     * win while hovered (topmost window draws last, so it takes priority) */
    {
        int cy2 = w->y + TITLE_H / 2;
        int nbtn = w->no_resize ? 2 : 3;
        for (int s = 0; s < nbtn; s++) {
            int slot = w->no_resize ? (s == 0 ? 1 : 2) : s;
            int bxp = w->x + w->w - 70 + slot * 24;
            if (g_mx >= bxp - 9 && g_mx < bxp + 9 &&
                g_my >= cy2 - 9 && g_my < cy2 + 9) {
                gui_set_cursor(CUR_HAND);
                break;
            }
        }
    }
}

/* ---------- RTC ---------- */

static uint8_t cmos_read(uint8_t reg) {
    __asm__ volatile("outb %0, $0x70" :: "a"(reg));
    uint8_t v;
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}
/* RTC register-B mode, latched on first read: bit1 DM (1=binary), bit2 24HR.
 * NEVER write register B - bit7 is the SET/update-inhibit flag and setting
 * it freezes the RTC (the old dead-clock bug). */
static int g_rtc_dm = -1, g_rtc_h24 = -1;
static void rtc_latch_mode(void) {
    if (g_rtc_dm >= 0) return;
    uint8_t rb = cmos_read(0x0B);
    g_rtc_dm = (rb & 0x02) ? 1 : 0;
    g_rtc_h24 = (rb & 0x04) ? 1 : 0;
}
static int rtc_bcd(uint8_t v) { return ((v >> 4) * 10) + (v & 0xF); }
static int rtc_bcd_enc(int v) { return ((v / 10) << 4) | (v % 10); }  /* 0..99 -> BCD */
static void cmos_write(uint8_t reg, uint8_t val) {
    __asm__ volatile("outb %0, $0x70" :: "a"(reg));
    __asm__ volatile("outb %0, $0x71" :: "a"(val));
}

/* seconds offset learned from SNTP: displayed_time = RTC_time + offset */
static int64_t g_time_offset;
static int g_time_synced;          /* 1 once SNTP/manual set gave a trusted time */
static uint64_t g_next_sntp;       /* pit_ticks() of next scheduled sync */
static int g_sntp_waiting;         /* a sync request is in flight */
static uint64_t g_sntp_t0;         /* when it started (timeout base) */
static uint64_t g_sntp_done;       /* pit_ticks() of last successful sync */
static uint64_t g_sntp_fail;       /* pit_ticks() of last failed sync */

/* days-from-civil / civil-from-days (Howard Hinnant's algorithms) - used
 * to apply the SNTP offset to the RTC's civil time */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = m > 2 ? m - 3 : m + 9;
    unsigned doy = (153 * mp + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int *yy, int *mm, int *dd) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mm = (int)(mp + (mp < 10 ? 3 : (unsigned)-9));
    *yy = (int)(y + (*mm <= 2));
}

/* decode one CMOS snapshot into 24h civil time (no offset applied).
 * Register B's DM bit is NOT always trustworthy (QEMU/SeaBIOS can report
 * binary while the silicon still returns BCD - the old code then decoded
 * 0x45 as 69 minutes). Decode BOTH ways, keep whichever produces sane
 * field ranges, and only trust the DM bit to break ties. */
static void rtc_decode(uint8_t sec, uint8_t min, uint8_t hour,
                       uint8_t dom, uint8_t mon, uint8_t yr,
                       int *h24, int *mi, int *ss, int *dd, int *mm, int *yy) {
    int hB = rtc_bcd(hour & 0x3F), mB = rtc_bcd(min), sB = rtc_bcd(sec);
    int dB = rtc_bcd(dom), moB = rtc_bcd(mon), yB = rtc_bcd(yr);
    int hN = hour & 0x3F, mN = min, sN = sec;
    int dN = dom, moN = mon, yN = yr;
    int vb = (mB < 60 && sB < 60 && moB >= 1 && moB <= 12 &&
              dB >= 1 && dB <= 31 && hB <= 23);
    int vn = (mN < 60 && sN < 60 && moN >= 1 && moN <= 12 &&
              dN >= 1 && dN <= 31 && hN <= 23);
    int use_bin = (vb && vn) ? g_rtc_dm : (vn && !vb);
    int h = use_bin ? hN : hB;
    int m = use_bin ? mN : mB;
    int s = use_bin ? sN : sB;
    int d = use_bin ? dN : dB;
    int mo = use_bin ? moN : moB;
    int y = use_bin ? yN : yB;
    int hh = h;
    if (!g_rtc_h24) {
        bool pm = (hour & 0x80) != 0;
        hh = h % 12; if (hh == 0) hh = 12;
        if (pm && hh != 12) hh += 12;
        if (!pm && hh == 12) hh = 0;
    }
    *h24 = hh; *mi = m; *ss = s; *dd = d; *mm = mo; *yy = y;
}

/* timezone + DST: displayed local time = UTC + tz_min + dst_offset.
 * tz comes from persistent settings; DST rules: US (2nd Sun Mar ->
 * 1st Sun Nov) and EU (last Sun Mar -> last Sun Oct), evaluated on the
 * UTC date each call so transitions are automatic. */
static int dow_civil(int64_t y, unsigned m, unsigned d) {
    /* 0=Sunday; days_from_civil counts from 1970-01-01 (Thursday=4) */
    int64_t z = days_from_civil(y, m, d);
    int w = (int)((z + 4) % 7);
    if (w < 0) w += 7;
    return w;
}
/* day-of-month of the nth (1-based) weekday w in month m; last=1 -> final */
static int nth_dow(int64_t y, unsigned m, int w, int n, int last) {
    if (last) {
        /* start from day 31 and walk back */
        int dim = (m == 4 || m == 6 || m == 9 || m == 11) ? 30 :
                  (m == 2 ? ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : 28) : 31);
        for (int d = dim; d >= 1; d--)
            if (dow_civil(y, m, (unsigned)d) == w) return d;
        return dim;
    }
    int first = 1;
    while (dow_civil(y, m, (unsigned)first) != w) first++;
    return first + (n - 1) * 7;
}
/* is DST in effect at UTC epoch z? rule: 0 none, 1 US, 2 EU, 3 AU.
 * AU is southern-hemisphere: DST wraps the year boundary (Oct -> Apr). */
static int dst_active_at(int64_t z, int rule) {
    if (!rule) return 0;
    int64_t days = z / 86400;
    int yy, mm, dd;
    civil_from_days(days, &yy, &mm, &dd);
    int64_t start_z, end_z;
    if (rule == 1) {   /* US: 2nd Sun Mar 07:00 UTC -> 1st Sun Nov 06:00 UTC */
        start_z = days_from_civil(yy, 3, (unsigned)nth_dow(yy, 3, 0, 2, 0)) * 86400 + 7 * 3600;
        end_z   = days_from_civil(yy, 11, (unsigned)nth_dow(yy, 11, 0, 1, 0)) * 86400 + 6 * 3600;
        return z >= start_z && z < end_z;
    }
    if (rule == 2) {   /* EU: last Sun Mar 01:00 UTC -> last Sun Oct 01:00 UTC */
        start_z = days_from_civil(yy, 3, (unsigned)nth_dow(yy, 3, 0, 1, 1)) * 86400 + 3600;
        end_z   = days_from_civil(yy, 10, (unsigned)nth_dow(yy, 10, 0, 1, 1)) * 86400 + 3600;
        return z >= start_z && z < end_z;
    }
    /* AU (Sydney): DST = 1st Sun Oct 02:00 AEST -> 1st Sun Apr 03:00 AEDT.
     * Both transitions occur at 16:00 UTC the day before the Sunday, i.e.
     * 8h before that Sunday's 00:00 UTC. */
    start_z = days_from_civil(yy, 10, (unsigned)nth_dow(yy, 10, 0, 1, 0)) * 86400 - 8 * 3600;
    end_z   = days_from_civil(yy, 4,  (unsigned)nth_dow(yy, 4,  0, 1, 0)) * 86400 - 8 * 3600;
    return z >= start_z || z < end_z;   /* wraps the new year */
}

/* raw RTC epoch (Unix seconds, 2-digit year -> 2000+y), NO offset - used
 * once per SNTP sync to compute g_time_offset */
static int64_t rtc_epoch_raw(void) {
    rtc_latch_mode();
    for (int i = 0; i < 400; i++) {
        __asm__ volatile("outb %0, $0x70" :: "a"((uint8_t)0x0A));
        uint8_t st;
        __asm__ volatile("inb $0x71, %0" : "=a"(st));
        if (!(st & 0x80)) break;
    }
    uint8_t sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    uint8_t dom = cmos_read(0x07), mon = cmos_read(0x08), yr = cmos_read(0x09);
    int h24, mi, ss, dd, mm, yy;
    rtc_decode(sec, min, hour, dom, mon, yr, &h24, &mi, &ss, &dd, &mm, &yy);
    return days_from_civil(2000 + yy, (unsigned)mm, (unsigned)dd) * 86400
         + (int64_t)h24 * 3600 + mi * 60 + ss;
}

void gui_rtc_time(char *buf, int size) { rtc_get_time(buf, (size_t)size); }

/* current local civil time as Unix epoch seconds: RTC + SNTP skew +
 * configured timezone + automatic DST. Single source of truth - both
 * the clock string and the calendar derive from this so they can never
 * disagree. */
static int64_t local_epoch(void) {
    rtc_latch_mode();
    /* wait for update-in-progress (reg 0x0A bit 7) to clear - bounded so a
     * stuck flag can never stall the UI */
    for (int i = 0; i < 400; i++) {
        __asm__ volatile("outb %0, $0x70" :: "a"((uint8_t)0x0A));
        uint8_t st;
        __asm__ volatile("inb $0x71, %0" : "=a"(st));
        if (!(st & 0x80)) break;
    }
    /* consistent snapshot: seconds is read FIRST and LAST - if the RTC
     * ticked mid-read the old code mixed old/new registers and produced
     * one-minute jumps ("12:40 -> 12:45"). Re-read on mismatch, 3 tries. */
    uint8_t sec = 0, min = 0, hour = 0, dom = 0, mon = 0, yr = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t s1 = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
        dom  = cmos_read(0x07);
        mon  = cmos_read(0x08);
        yr   = cmos_read(0x09);
        sec  = cmos_read(0x00);
        if (s1 == sec) break;
    }
    int h24, m, s, d, mo, y;
    rtc_decode(sec, min, hour, dom, mon, yr, &h24, &m, &s, &d, &mo, &y);
    {
        static bool rtc_dbg;
        if (!rtc_dbg) {
            rtc_dbg = true;
            kprintf("rtc: raw %02x:%02x:%02x h24=%d %d/%d/%02d dm=%d h24mode=%d\n",
                    min, sec, hour, h24, d, mo, y, g_rtc_dm, g_rtc_h24);
        }
    }
    /* SNTP skew turns RTC civil time into true UTC; then apply the
     * configured timezone + automatic DST on top */
    int64_t utc = days_from_civil(2000 + y, (unsigned)mo, (unsigned)d) * 86400
                + (int64_t)h24 * 3600 + m * 60 + s + g_time_offset;
    int16_t tz_min; uint8_t dst_rule, tz_flags;
    settings_get_tz(&tz_min, &dst_rule, &tz_flags);
    if (!tz_flags) { tz_min = 600; dst_rule = 3; }  /* default: Australia Eastern (AEDT/AEST) */
    return utc + (int64_t)tz_min * 60 +
           (dst_active_at(utc, dst_rule) ? 3600 : 0);
}

void rtc_get_time(char *buf, size_t size) {
    /* cache the CMOS read: port IO is expensive in a VM, and reading every
     * frame caused multi-hundred-ms stalls (the "frozen clock"). 4 reads/sec
     * is plenty - the seconds digit still ticks on time. */
    static char cached[40];
    static uint64_t last_read;
    uint64_t now = pit_ticks();
    if (cached[0] && now - last_read < pit_hz() / 4) {
        strncpy(buf, cached, size - 1);
        buf[size - 1] = 0;
        return;
    }
    last_read = now;
    /* port 0x70/0x71 needs no NMI masking on PC; register B is only READ
     * (writing bit7 SET would freeze the RTC - the dead-clock bug) */
    int64_t epoch = local_epoch();
    int64_t zdays = epoch / 86400;
    int64_t rem = epoch - zdays * 86400;
    int hh = (int)(rem / 3600);
    int m = (int)((rem / 60) % 60);
    int s = (int)(rem % 60);
    int y, mo, d;
    civil_from_days(zdays, &y, &mo, &d);
    y -= 2000; if (y < 0) y = 0;
    /* 12-hour display fields */
    const char *ap = (hh >= 12) ? "PM" : "AM";
    int h = hh % 12; if (h == 0) h = 12;
    /* DD/MM/YY H:MM:SS AM - date first, clock ticks every second */
    char tmp[40];
    int i = 0;
    tmp[i++] = (char)('0' + d / 10); if (d < 10) tmp[i-1] = ' ';
    tmp[i++] = (char)('0' + d % 10);
    tmp[i++] = '/';
    tmp[i++] = (char)('0' + mo / 10); if (mo < 10) tmp[i-1] = ' ';
    tmp[i++] = (char)('0' + mo % 10);
    tmp[i++] = '/';
    tmp[i++] = (char)('0' + y / 10);
    tmp[i++] = (char)('0' + y % 10);
    tmp[i++] = ' ';
    tmp[i++] = (char)('0' + h / 10); if (h < 10) tmp[i-1] = ' ';
    tmp[i++] = (char)('0' + h % 10);
    tmp[i++] = ':';
    tmp[i++] = (char)('0' + m / 10);
    tmp[i++] = (char)('0' + m % 10);
    tmp[i++] = ':';
    tmp[i++] = (char)('0' + s / 10);
    tmp[i++] = (char)('0' + s % 10);
    tmp[i++] = ' ';
    tmp[i++] = ap[0]; tmp[i++] = ap[1];
    tmp[i] = 0;
    strncpy(cached, tmp, sizeof(cached) - 1);
    cached[sizeof(cached) - 1] = 0;
    strncpy(buf, tmp, size - 1);
    buf[size - 1] = 0;
}

/* structured RTC date for the calendar popup: y=full year, m=1-12, d=day,
 * wd=0=Sunday. Same CMOS read + caching strategy as rtc_get_time. */
void rtc_date_parts(int *yy, int *mm, int *dd, int *wd) {
    static int cy = -1, cm, cd, cw;
    static uint64_t last;
    uint64_t now = pit_ticks();
    if (cy >= 0 && now - last < pit_hz() / 4) {
        *yy = cy; *mm = cm; *dd = cd; *wd = cw; return;
    }
    last = now;
    /* same local_epoch() source as rtc_get_time: timezone + DST applied,
     * so the calendar can never disagree with the menu-bar clock */
    int64_t epoch = local_epoch();
    int64_t zdays = epoch / 86400;
    cy = 0; cm = 0; cd = 0;
    civil_from_days(zdays, &cy, &cm, &cd);
    cw = (int)((zdays + 4) % 7);           /* 1970-01-01 = Thursday */
    if (cw < 0) cw += 7;                   /* 0=Sunday */
    *yy = cy; *mm = cm; *dd = cd; *wd = cw;
}

/* ---- time services for Settings / apps ---- */

/* current LOCAL civil time (post timezone+DST) as 24h h:m:s.
 * Cached at 4Hz like rtc_get_time - local_epoch() does CMOS port IO
 * and a full DST evaluation; calling it every frame would stall the UI. */
static int g_hms_gen;   /* bumped on manual RTC set: invalidates the
                        * gui_rtc_local_hms cache so steppers show the
                        * new value on the very next frame (no jitter) */
void gui_rtc_local_hms(int *h, int *m, int *s) {
    static int ch = -1, cm, cs, gen = -1;
    static uint64_t last;
    uint64_t now = pit_ticks();
    if (ch >= 0 && gen == g_hms_gen && now - last < pit_hz() / 4) {
        if (h) *h = ch; if (m) *m = cm; if (s) *s = cs; return;
    }
    last = now; gen = g_hms_gen;
    int64_t epoch = local_epoch();
    int64_t days = epoch / 86400;
    int64_t rem = epoch - days * 86400;
    if (rem < 0) { rem += 86400; days--; }
    ch = (int)(rem / 3600);
    cm = (int)((rem / 60) % 60);
    cs = (int)(rem % 60);
    if (h) *h = ch; if (m) *m = cm; if (s) *s = cs;
}

/* set the clock to a LOCAL wall time (h:m:s today, date unchanged).
 * Inverts local_epoch(): RTC stores the pre-tz instant, so we back out
 * the timezone + DST + SNTP skew and write the resulting UTC fields. */
void gui_rtc_set_local(int hh, int mm, int ss) {
    rtc_latch_mode();
    if (hh < 0) hh = 0; if (hh > 23) hh = 23;
    if (mm < 0) mm = 0; if (mm > 59) mm = 59;
    if (ss < 0) ss = 0; if (ss > 59) ss = 59;
    int64_t le = local_epoch();
    int64_t zdays = le / 86400;
    int64_t rem = le - zdays * 86400;
    if (rem < 0) { rem += 86400; zdays--; }
    int64_t target = zdays * 86400 + (int64_t)hh * 3600 + mm * 60 + ss;
    int16_t tz; uint8_t dr, fl;
    settings_get_tz(&tz, &dr, &fl);
    if (!fl) { tz = 600; dr = 3; }
    int64_t utc = target - (int64_t)tz * 60;
    int64_t rtc_ep = utc - (dst_active_at(utc, dr) ? 3600 : 0) - g_time_offset;
    int64_t rd = rtc_ep / 86400;
    int64_t rr = rtc_ep - rd * 86400;
    if (rr < 0) { rr += 86400; rd--; }
    int ry, rm2, rdd;
    civil_from_days(rd, &ry, &rm2, &rdd);
    int rh = (int)(rr / 3600), rmi = (int)((rr / 60) % 60), rs = (int)(rr % 60);
    /* halt RTC updates while writing (reg B bit7 SET), restore after */
    for (int i = 0; i < 400; i++) {
        __asm__ volatile("outb %0, $0x70" :: "a"((uint8_t)0x0A));
        uint8_t st; __asm__ volatile("inb $0x71, %0" : "=a"(st));
        if (!(st & 0x80)) break;
    }
    uint8_t rb = cmos_read(0x0B);
    cmos_write(0x0B, rb | 0x80);
    int dm = g_rtc_dm, h24 = g_rtc_h24;
    if (dm < 0) dm = 0; if (h24 < 0) h24 = 1;
    cmos_write(0x00, dm ? (uint8_t)rs : (uint8_t)rtc_bcd_enc(rs));
    cmos_write(0x02, dm ? (uint8_t)rmi : (uint8_t)rtc_bcd_enc(rmi));
    /* hour honours the RTC's 12/24h mode; PM lives in bit7 (0x80) */
    uint8_t hreg;
    if (h24) {
        hreg = dm ? (uint8_t)rh : (uint8_t)rtc_bcd_enc(rh);
    } else {
        int h12 = rh % 12; if (h12 == 0) h12 = 12;
        int pm = (rh >= 12) ? 0x80 : 0;
        hreg = dm ? (uint8_t)(pm | h12) : (uint8_t)(pm | rtc_bcd_enc(h12));
    }
    cmos_write(0x04, hreg);
    /* date too: the local->UTC back-out can cross midnight, so keep the
     * RTC date consistent with the instant we just wrote */
    cmos_write(0x07, dm ? (uint8_t)rdd : (uint8_t)rtc_bcd_enc(rdd));
    cmos_write(0x08, dm ? (uint8_t)rm2 : (uint8_t)rtc_bcd_enc(rm2));
    cmos_write(0x09, dm ? (uint8_t)(ry % 100) : (uint8_t)rtc_bcd_enc(ry % 100));
    cmos_write(0x0B, rb & 0x7F);
    g_time_synced = 1;   /* user-set clock counts as a trusted time source */
    g_hms_gen++;         /* drop the read cache: next read sees new time */
    kprintf("rtc: manual set local %02d:%02d:%02d (rtc %02d:%02d:%02d)\n",
            hh, mm, ss, rh, rmi, rs);
}

int gui_time_synced(void) { return g_time_synced; }
void gui_sntp_sync_now(void) { g_next_sntp = 0; }
/* 0 idle, 1 syncing, 2 just synced, 3 just failed - for Settings UI */
int gui_time_sync_state(void) {
    uint64_t now = pit_ticks();
    if (g_sntp_waiting) return 1;
    if (g_sntp_done && now - g_sntp_done < pit_hz() * 4) return 2;
    if (g_sntp_fail && now - g_sntp_fail < pit_hz() * 4 &&
        (!g_sntp_done || g_sntp_fail > g_sntp_done)) return 3;
    return 0;
}

/* ---------- input ---------- */

static int g_net_busy;   /* nonzero while a blocking net call yields: keep
                          * cursor/mouse alive but skip app event delivery */
static int g_desk_drag = -1;          /* icon being drag-repositioned */
static int g_desk_drag_sx, g_desk_drag_sy;
static int16_t g_drag_base_x[64], g_drag_base_y[64]; /* offsets at drag start */
static bool g_desk_dragging;
static int g_marquee;                 /* rubber-band selection active */
static int g_mar_x0, g_mar_y0, g_mar_x1, g_mar_y1;
static int g_resize_win = -1;         /* window being resized via bottom-right grip */
static int g_rs_x, g_rs_y, g_rs_w, g_rs_h;

/* rubber-band selection mask: bit i = icon i touched by the marquee rect */
static uint64_t marquee_mask(void) {
    int x0 = g_mar_x0 < g_mar_x1 ? g_mar_x0 : g_mar_x1;
    int y0 = g_mar_y0 < g_mar_y1 ? g_mar_y0 : g_mar_y1;
    int x1 = g_mar_x0 > g_mar_x1 ? g_mar_x0 : g_mar_x1;
    int y1 = g_mar_y0 > g_mar_y1 ? g_mar_y0 : g_mar_y1;
    if (x1 - x0 < 4 || y1 - y0 < 4) return 0;
    uint64_t mask = 0;
    for (int i = 0; i < desk_icon_count(); i++) {
        int ix, iy; desk_icon_rect(i, &ix, &iy);
        int isz = g_desk_isz;
        if (ix < x1 && ix + isz + 8 > x0 &&
            iy < y1 + 30 && iy + isz + 30 > y0)
            mask |= 1ULL << i;
    }
    return mask;
}
static int g_drag_win = -1;
static int g_mouse_win = -1; /* window that received mouse_down */
static uint64_t g_last_click_ms;
static int g_last_click_win = -1;
static int g_last_desk_click = -1;
static uint64_t g_last_desk_ms;
static int g_wheel_accum;
static char g_pending_file[VFS_NAME_LEN + 1];  /* file Quill should open */
static char g_pending_dir[VFS_NAME_LEN + 1];   /* folder it lives in ("" = root) */
static char g_pending_folder[VFS_NAME_LEN + 1]; /* folder Files should show */
static char g_pending_sys[128];    /* ISO system-volume path Quill should open */
static char g_pending_img[160];    /* "dir/name" PNG Photos should open */
static char g_pending_sysimg[128]; /* ISO sysvol path of a PNG for Photos */
static char g_pending_audio[192];  /* track path Voyage Player should play */
const char *gui_pending_audio(void) { return g_pending_audio[0] ? g_pending_audio : NULL; }
void gui_clear_pending_audio(void) { g_pending_audio[0] = 0; }
void gui_set_pending_audio(const char *path) {
    g_pending_audio[0] = 0;
    if (!path || !path[0]) return;
    strncpy(g_pending_audio, path, sizeof(g_pending_audio) - 1);
    g_pending_audio[sizeof(g_pending_audio) - 1] = 0;
}
const char *gui_pending_sysimg(void) { return g_pending_sysimg[0] ? g_pending_sysimg : NULL; }
void gui_clear_pending_sysimg(void) { g_pending_sysimg[0] = 0; }
void gui_set_pending_sysimg(const char *path) {
    g_pending_sysimg[0] = 0;
    if (!path || !path[0]) return;
    strncpy(g_pending_sysimg, path, sizeof(g_pending_sysimg) - 1);
    g_pending_sysimg[sizeof(g_pending_sysimg) - 1] = 0;
    g_pending_img[0] = 0; g_pending_sys[0] = 0;
}
const char *gui_pending_image(void) { return g_pending_img[0] ? g_pending_img : NULL; }
void gui_clear_pending_image(void) { g_pending_img[0] = 0; g_pending_sysimg[0] = 0; }
void gui_set_pending_image(const char *dir, const char *name) {
    g_pending_img[0] = 0;
    g_pending_sysimg[0] = 0;
    if (!name || !name[0]) return;
    snprintf(g_pending_img, sizeof(g_pending_img), "%s%s%s",
             dir ? dir : "", (dir && dir[0]) ? "/" : "", name);
    g_pending_sys[0] = 0;
}
const char *gui_pending_sysfile(void) { return g_pending_sys[0] ? g_pending_sys : NULL; }
void gui_clear_pending_sysfile(void) { g_pending_sys[0] = 0; }
void gui_set_pending_sysfile(const char *path) {
    if (!path) { g_pending_sys[0] = 0; return; }
    g_pending_file[0] = 0; g_pending_dir[0] = 0;   /* mutually exclusive */
    strncpy(g_pending_sys, path, sizeof(g_pending_sys) - 1);
    g_pending_sys[sizeof(g_pending_sys) - 1] = 0;
}
const char *gui_pending_file(void) { return g_pending_file[0] ? g_pending_file : NULL; }
void gui_clear_pending_file(void) { g_pending_file[0] = 0; g_pending_dir[0] = 0; g_pending_sys[0] = 0; }
void gui_set_pending_file(const char *name) {
    if (!name) { g_pending_file[0] = 0; g_pending_dir[0] = 0; g_pending_sys[0] = 0; return; }
    g_pending_sys[0] = 0;
    strncpy(g_pending_file, name, sizeof(g_pending_file) - 1);
    g_pending_file[sizeof(g_pending_file) - 1] = 0;
    g_pending_dir[0] = 0;
}
const char *gui_pending_dir(void) { return g_pending_dir; }
void gui_set_pending_path(const char *dir, const char *name) {
    gui_set_pending_file(name);
    if (dir) {
        strncpy(g_pending_dir, dir, sizeof(g_pending_dir) - 1);
        g_pending_dir[sizeof(g_pending_dir) - 1] = 0;
    }
}
const char *gui_pending_folder(void) { return g_pending_folder[0] ? g_pending_folder : NULL; }
void gui_clear_pending_folder(void) { g_pending_folder[0] = 0; }
void gui_set_pending_folder(const char *name) {
    if (!name) { g_pending_folder[0] = 0; return; }
    strncpy(g_pending_folder, name, sizeof(g_pending_folder) - 1);
    g_pending_folder[sizeof(g_pending_folder) - 1] = 0;
}

static int window_at(int mx, int my) {
    /* topmost = highest z among windows containing the point */
    int best = -1;
    for (int i = 0; i < g_win_count; i++) {
        Window *w = &g_win[i];
        if (!w->visible || w->minimized || w->closing) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            if (best < 0 || w->z > g_win[best].z) best = i;
        }
    }
    return best;
}

static void send_event(Window *w, GuiEvent *ev) {
    if (!w || !w->app || !w->app->event || !w->ctx) return;
    g_cur_win = w;
    g_cur_cx = content_x(w);
    g_cur_cy = content_y(w);
    ev->x = ev->gx - g_cur_cx;
    ev->y = ev->gy - g_cur_cy;
    w->app->event(w->ctx, ev);
    g_cur_win = NULL;
}

/* open a dock-pinned VFS file/folder: folders -> Files, .txt -> Quill,
 * .png -> Photos, .lnk -> its app, anything else -> invalid message */
/* audio file? .mp3/.wav/.ogg case-insensitive - shared by icon map + launchers */
int gui_is_audio(const char *name) {
    int l = (int)strlen(name);
    return l > 4 && (!strcasecmp(name + l - 4, ".mp3") ||
                     !strcasecmp(name + l - 4, ".wav") ||
                     !strcasecmp(name + l - 4, ".ogg"));
}
/* open Voyage Player queued on `path` (VFS name or sysvol "dir/name") */
static void gui_launch_audio(const char *path) {
    if (!path || !path[0]) return;
    gui_set_pending_audio(path);
    App *a = gui_find_app("Voyage Player");
    if (a) gui_open_app_new(a);
}

static void dock_open_vfs(struct DockEnt *e) {
    int fi = e->dir[0] ? vfs_find_in(e->dir, e->name) : vfs_find(e->name);
    const VfsEntry *ve = fi >= 0 ? vfs_entry(fi) : NULL;
    if (!ve) return;
    if (ve->type == VFS_DIR) {
        if (e->dir[0]) return;   /* nested folders: open Files root view */
        gui_set_pending_folder(e->name);
        App *f = gui_find_app("Files");
        if (f) gui_open_app_new(f);
        return;
    }
    if (ve->system) { gui_deny(); return; }
    int nl = (int)strlen(e->name);
    bool istxt = nl > 4 && !strcmp(e->name + nl - 4, ".txt");
    bool ispng = gui_is_png(e->name);
    bool islnk = nl > 4 && !strcmp(e->name + nl - 4, ".lnk");
    if (islnk) {
        char buf[64];
        if (vfs_read_in(e->dir, e->name, buf, sizeof(buf) - 1) > 0) {
            buf[sizeof(buf) - 1] = 0;
            App *a = gui_find_app(buf);
            if (a) { gui_open_app_new(a); return; }
        }
        gui_notify("This file is invalid or corrupted!");
        return;
    }
    if (istxt) {
        gui_set_pending_path(e->dir, e->name);
        App *q = gui_find_app("Quill");
        if (q) gui_open_app_new(q);
    } else if (ispng) {
        gui_set_pending_image(e->dir, e->name);
        gui_open_app_new(app_make_photos());
    } else if (gui_is_audio(e->name)) {
        /* audio inside a folder too - "dir/name" path lets the player
         * match folder tracks, not just desktop-root files */
        char apath[192];
        if (e->dir[0]) snprintf(apath, sizeof(apath), "%s/%s", e->dir, e->name);
        else snprintf(apath, sizeof(apath), "%s", e->name);
        gui_launch_audio(apath);
    } else {
        gui_notify("This file is invalid or corrupted!");
    }
}

/* plain dock-icon click behavior (open/focus/minimize/popup) - runs on
 * mouse RELEASE so a press-and-drag can pull the app out as a shortcut.
 * `ent` is a DOCK ENTRY index, not an app index. */
static void dock_icon_click(int ent) {
    if (ent < 0 || ent >= g_dock_n) return;
    struct DockEnt *de = &g_dock[ent];
    if (de->kind == 1) { dock_open_vfs(de); return; }
    App *a = g_apps[de->app];
    int cnt = app_window_count(a);
    if (cnt == 0) {
        gui_open_app_new(a);
    } else if (cnt > 1) {
        /* multiple instances: toggle the window-list popup */
        if (g_pop_app == de->app) {
            if (g_pop_x_t) { g_pop_app = -1; g_pop_x_t = 0; }
            else popup_start_close(3);
        } else {
            g_pop_app = de->app; g_pop_x_t = 0;
            g_pop_scroll = 0; g_pop_dock = ent;
        }
    } else if (app_focused_idx(a) >= 0) {
        /* clicking the focused app's icon minimizes ALL its windows -
         * through minimize_window_idx so each one SPINS OUT first,
         * exactly like the caption _ button */
        for (int i = 0; i < g_win_count; i++)
            if (g_win[i].app == a && !g_win[i].minimized)
                minimize_window_idx(i);
        g_focus = -1;
        for (int i = g_win_count - 1; i >= 0; i--)
            if (g_win[i].visible && !g_win[i].minimized) {
                focus_window(i); break;
            }
    } else {
        /* open but not focused: restore minimized windows, focus topmost */
        int top = -1;
        for (int i = 0; i < g_win_count; i++)
            if (g_win[i].app == a) {
                restore_window_spin(i);
                if (top < 0 || g_win[i].z > g_win[top].z) top = i;
            }
        if (top >= 0) focus_window(top);
    }
}

/* ---------- power: ACPI shutdown + keyboard-controller reboot ---------- */

static inline void gui_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t gui_inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void gui_outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port));
}

void gui_poweroff(void) {
    kprintf("voyager: powering off\n");
    /* ACPI: scan BIOS area for RSDP -> RSDT -> FADT, then write
     * SLP_TYP=S5|SLP_EN to PM1a_CNT (transplanted from Retro 95) */
    volatile uint8_t *rsdp = NULL;
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        volatile uint8_t *p = (volatile uint8_t *)kphys_to_virt(a);
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[8] == 'P' && p[9] == 'T' && p[10] == 'R' && p[11] == ' ') {
            rsdp = p; break;
        }
    }
    if (rsdp) {
        uint32_t rsdt_pa = *(volatile uint32_t *)(rsdp + 16);
        volatile uint8_t *rsdt = (volatile uint8_t *)kphys_to_virt(rsdt_pa);
        uint32_t count = *(volatile uint32_t *)(rsdt + 36);
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint32_t ent_pa = *(volatile uint32_t *)(rsdt + 40 + i * 4);
            volatile uint8_t *ent = (volatile uint8_t *)kphys_to_virt(ent_pa);
            if (ent[0] == 'F' && ent[1] == 'A' && ent[2] == 'C' && ent[3] == 'P') {
                uint8_t pm_space = ent[76];
                uint64_t pm_addr = *(volatile uint64_t *)(ent + 80);
                uint32_t val = 0x2000u | (5u << 10);   /* SLP_EN | S5 */
                if (pm_space == 0) {
                    gui_outl((uint16_t)pm_addr, val);
                } else {
                    volatile uint16_t *pm =
                        (volatile uint16_t *)kphys_to_virt(pm_addr);
                    *pm = (uint16_t)val;
                }
                break;
            }
        }
    }
    for (;;) __asm__ volatile("hlt");
}

void gui_reboot(void) {
    kprintf("voyager: rebooting\n");
    while (gui_inb(0x64) & 2u) { }
    gui_outb(0x64, 0xFE);   /* keyboard-controller reset pulse */
    for (;;) { }
}

static void pump_input(void) {
    mouse_poll();   /* drain PS/2 mouse FIFO - works even if IRQ12 never fires */
    net_poll();     /* drain the NIC every frame: async traffic (SNTP time
                     * sync, DHCP renewals, stray TCP) only gets processed
                     * when something polls - without this the clock sync
                     * replies sat in the ring forever */
    int32_t mx, my;
    uint8_t btn;
    mouse_get(&mx, &my, &btn);
    /* clamp */
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    if ((uint32_t)mx >= fb_width()) mx = fb_width() - 1;
    if ((uint32_t)my >= fb_height()) my = fb_height() - 1;
    g_wheel_accum += mouse_wheel_delta();

    /* during a blocking network call we still track the pointer (so the
     * cursor/dock stay alive) but must not deliver events into app code -
     * that would re-enter whatever is mid-fetch. */
    if (g_net_busy) {
        g_mx = mx; g_my = my; g_mbuttons = btn;
        return;
    }

    int win = window_at(mx, my);

    /* mouse move */
    static int32_t last_mx = -1, last_my = -1;
    if (mx != last_mx || my != last_my) {
        last_mx = mx; last_my = my;
        int target = (g_drag_win >= 0) ? g_drag_win : (g_mouse_win >= 0 ? g_mouse_win : win);
        if (target >= 0 && target < g_win_count) {
            Window *w = &g_win[target];
            bool inside = mx >= content_x(w) && mx < content_x(w) + content_w(w) &&
                          my >= content_y(w) && my < content_y(w) + content_h(w);
            /* while the left button is held on this window keep feeding
             * moves even outside its rect - lets apps drag items out onto
             * the desktop (coords go negative, apps must tolerate that) */
            if (inside || ((g_mbuttons & 1) && g_mouse_win == target)) {
                GuiEvent ev = {0};
                ev.type = EV_MOUSE_MOVE; ev.gx = mx; ev.gy = my;
                send_event(w, &ev);
            }
        }
        /* alert dialog titlebar drag (modal, moves like a window) */
        if (g_alert.active && g_alert.dragging) {
            if (!(btn & 1)) g_alert.dragging = 0;
            else {
                g_alert.ox = mx - g_alert.gx0 - g_alert.bx0;
                g_alert.oy = my - g_alert.gy0 - g_alert.by0;
            }
        }
    }

    /* buttons */
    /* volume slider drag: track the pointer while left button held */
    if (g_vol_drag) {
        if (!(btn & 1)) g_vol_drag = 0;
        else {
            int v = (mx - g_vol_x0) * 100 / (g_vol_x1 - g_vol_x0);
            ac97_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        }
    }
    uint8_t changed = btn ^ g_mbuttons;
    if (changed & 1) { /* left */
        if (btn & 1) {
            /* boot splash: swallow clicks so nothing invisible launches */
            if (pit_ticks() < g_splash_until) goto done_down;
            /* modal "Do you want to save?" dialog eats clicks first */
            if (g_save_win >= 0 && save_dialog_click(mx, my)) goto done_down;
            /* system alert (OOM warning / force-kill notice) is modal too */
            if (alert_click(mx, my)) goto done_down;
            /* calendar is an OVERLAY, not a window: while open, every
             * click is swallowed so it can never fall through to icons or
             * windows drawn beneath the panel. Inside -> month nav;
             * outside (except the clock toggle) -> just dismiss. */
            if (g_cal_open) {
                int inpanel = (mx >= g_cal_x && mx < g_cal_x + g_cal_w &&
                               my >= g_cal_y && my < g_cal_y + g_cal_h);
                int onclock = (my < MENU_H && mx >= g_clk_x0 && mx <= g_clk_x1);
                if (inpanel) { calendar_click(mx, my); goto done_down; }
                if (!onclock) { popup_start_close(0); goto done_down; }
            }
            /* Voyager dropdown + network popup: overlay panels like the
             * calendar - while open every click is swallowed so it can't
             * fall through to icons/windows beneath. Inside -> action;
             * on the icon itself -> fall through to toggle; elsewhere ->
             * dismiss only. */
            if (g_voy_open) {
                bool inpanel = (mx >= g_voy_x && mx < g_voy_x + g_voy_w &&
                                my >= g_voy_y && my < g_voy_y + g_voy_h);
                if (inpanel) {
                    for (int i = 0; i < 2; i++) {
                        if (mx >= g_voy_hit[i][0] && mx < g_voy_hit[i][2] &&
                            my >= g_voy_hit[i][1] && my < g_voy_hit[i][3]) {
                            popup_start_close(1);
                            if (i == 0) gui_poweroff(); else gui_reboot();
                            goto done_down;
                        }
                    }
                    goto done_down;   /* panel padding: swallow */
                }
                bool onvoy = (my < MENU_H && mx >= g_voy_x0 && mx <= g_voy_x1);
                if (!onvoy) { popup_start_close(1); goto done_down; }
            }
            if (g_net_open) {
                bool inpanel = (mx >= g_net_x && mx < g_net_x + g_net_w &&
                                my >= g_net_y && my < g_net_y + g_net_h);
                bool onicon = (my < MENU_H && mx >= g_net_x0 && mx <= g_net_x1);
                if (!inpanel && !onicon) { popup_start_close(2); goto done_down; }
                if (inpanel) goto done_down;   /* info panel: clicks do nothing */
            }
            /* mouse down */
            uint64_t now = pit_ticks();
            bool dbl = (g_last_click_win == win && now - g_last_click_ms < 500);
            g_last_click_ms = now;
            g_last_click_win = win;

            /* menu bar: Voyager dropdown, network popup, clock calendar,
             * volume slider drag */
            if (my < MENU_H) {
                g_mouse_win = -1;
                if (mx >= g_voy_x0 && mx <= g_voy_x1) {
                    if (g_voy_open) {
                        if (g_voy_x_t) { g_voy_open = 0; g_voy_x_t = 0; }
                        else popup_start_close(1);
                    } else {
                        g_voy_open = 1; g_voy_open_t = pit_ticks(); g_voy_x_t = 0;
                        if (g_net_open) popup_start_close(2);
                        if (g_cal_open) popup_start_close(0);
                    }
                } else if (mx >= g_net_x0 && mx <= g_net_x1) {
                    if (g_net_open) {
                        if (g_net_x_t) { g_net_open = 0; g_net_x_t = 0; }
                        else popup_start_close(2);
                    } else {
                        g_net_open = 1; g_net_open_t = pit_ticks(); g_net_x_t = 0;
                        if (g_voy_open) popup_start_close(1);
                        if (g_cal_open) popup_start_close(0);
                    }
                } else if (mx >= g_clk_x0 && mx <= g_clk_x1) {
                    if (g_cal_open) {
                        if (g_cal_x_t) { g_cal_open = 0; g_cal_x_t = 0; }
                        else popup_start_close(0);
                    } else {
                        g_cal_open = 1; g_cal_moff = 0;
                        g_cal_open_t = pit_ticks(); g_cal_x_t = 0;
                        if (g_voy_open) popup_start_close(1);
                        if (g_net_open) popup_start_close(2);
                    }
                } else if (mx >= g_vol_x0 && mx <= g_vol_x1 &&
                           my >= g_vol_y0 && my <= g_vol_y1) {
                    g_vol_drag = 1;
                    int v = (mx - g_vol_x0) * 100 / (g_vol_x1 - g_vol_x0);
                    ac97_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
                } else if (g_cal_open &&
                           !(mx >= g_cal_x && mx < g_cal_x + g_cal_w &&
                             my >= g_cal_y && my < g_cal_y + g_cal_h)) {
                    popup_start_close(0);   /* click outside closes it */
                }
            } else {
                /* instance popup open? clicking a row restores that window;
                 * clicking anywhere else just dismisses the popup */
                if (g_pop_app >= 0) {
                    bool inpop = (mx >= g_pop_x && mx < g_pop_x + g_pop_w &&
                                  my >= g_pop_y && my < g_pop_y + g_pop_h);
                    if (inpop) {
                        int r = (my - g_pop_y - 28) / 24;
                        if (r >= 0 && r < g_pop_rows) {
                            int wi = g_pop_item[r];
                            if (wi >= 0 && wi < g_win_count &&
                                g_win[wi].app == g_apps[g_pop_app]) {
                                restore_window_spin(wi);
                                focus_window(wi);
                            }
                        }
                        popup_start_close(3);
                        g_mouse_win = -1;
                        goto done_down;
                    }
                    popup_start_close(3);   /* dismissed - fall through */
                }
                /* context menu open? item click creates, outside click closes */
                if (g_ctx_open) {
                    bool in = (mx >= g_ctx_x && mx < g_ctx_x + g_ctx_w &&
                               my >= g_ctx_y && my < g_ctx_y + g_ctx_h);
                    if (in) {
                        int r = (my - g_ctx_y - 26) / 24;
                        if (r >= 0 && r < g_ctx_nitems) ctx_create(r);
                        g_ctx_open = 0;
                        g_mouse_win = -1;
                        goto done_down;
                    }
                    g_ctx_open = 0;
                }
                /* dock context menu open? row 0 = Close All (apps),
                 * last row = Remove from Dock; outside just dismisses */
                if (g_dock_menu >= 0) {
                    bool in = (mx >= g_dock_menu_x && mx < g_dock_menu_x + g_dock_menu_w &&
                               my >= g_dock_menu_y && my < g_dock_menu_y + g_dock_menu_h);
                    if (in) {
                        int row = (my - g_dock_menu_y - 4) / 24;
                        int n = g_dock_menu_app ? 2 : 1;
                        if (row == 0 && g_dock_menu_app &&
                            g_dock_menu >= 0 && g_dock_menu < g_dock_n &&
                            g_dock[g_dock_menu].kind == 0)
                            close_all_app_windows(g_apps[g_dock[g_dock_menu].app]);
                        else if (row == n - 1)
                            dock_remove(g_dock_menu);
                    }
                    g_dock_menu = -1;
                    g_mouse_win = -1;
                    goto done_down;
                }
                int di = dock_hit(mx, my);
                if (di == -2 || di == -3) {
                    /* dock scroll buttons: page through pinned entries */
                    struct DockLay L; dock_layout(mx, my, &L);
                    int page = L.vis > 1 ? L.vis - 1 : 1;
                    g_dock_scroll += (di == -3 ? page : -page);
                    if (g_dock_scroll < 0) g_dock_scroll = 0;
                    g_mouse_win = -1;
                } else if (di >= 0) {
                    /* press-and-hold: dragging past 6px pulls an APP out
                     * as a ".lnk" shortcut; a plain release clicks it
                     * (open/focus/minimize) on mouse-up instead */
                    g_dock_press = di;
                    g_dock_press_x = mx; g_dock_press_y = my;
                    g_dock_out = false;
                    g_mouse_win = -1;
                } else if (win >= 0) {
                    Window *w = &g_win[win];
                    focus_window(win);
                    int ty = my - w->y;
                    if (ty < TITLE_H) {
                        /* titlebar: right-side buttons min/zoom/close
                         * (zoom slot skipped on non-resizable apps) */
                        int cx = mx - w->x;
                        int cy = TITLE_H / 2;
                        int nbtn = w->no_resize ? 2 : 3;
                        for (int s = 0; s < nbtn; s++) {
                            int pos = w->no_resize ? s + 1 : s;
                            int slot = w->no_resize ? (s == 0 ? 0 : 2) : s;
                            int btnx = w->w - 70 + pos * 24;
                            int dx = cx - btnx, dy = ty - cy;
                            if (dx >= -8 && dx <= 8 && dy >= -8 && dy <= 8) {
                                if (slot == 0) minimize_window_idx(win);
                                else if (slot == 1) {
                                    /* zoom = boing to fullscreen (or back):
                                     * snapshot + animated scale with an
                                     * edge overshoot, maximized flag flips
                                     * when the animation lands */
                                    w->maximized = !w->maximized;
                                    zoom_window_anim(win, w->maximized);
                                } else close_window_idx(win);
                                g_mouse_win = -1;
                                goto done_down;
                            }
                        }
                        /* drag: pulling a maximized window's titlebar
                         * restores it to a normal window first */
                        if (w->maximized) {
                            w->maximized = false;
                            w->w = w->min_w; w->h = w->min_h;
                            w->x = mx - w->w / 2; w->y = my - 12;
                            /* this becomes the frame O restores to */
                            w->rx = w->x; w->ry = w->y;
                            w->rw = w->w; w->rh = w->h;
                        }
                        w->dragging = true;
                        w->drag_ox = mx - w->x;
                        w->drag_oy = my - w->y;
                        g_drag_win = win;
                        g_mouse_win = -1;
                    } else {
                        /* bottom-right grip: start a resize drag instead
                         * of delivering a content click */
                        if (!w->maximized && !w->no_resize &&
                            mx >= w->x + w->w - 18 &&
                            my >= w->y + w->h - 14) {
                            g_resize_win = win;
                            g_rs_x = mx; g_rs_y = my;
                            g_rs_w = w->w; g_rs_h = w->h;
                            g_mouse_win = -1;
                            goto done_down;
                        }
                        /* content click */
                        g_mouse_win = win;
                        GuiEvent ev = {0};
                        ev.type = EV_MOUSE_DOWN; ev.gx = mx; ev.gy = my; ev.button = 0;
                        send_event(w, &ev);
                        /* desktop icon double-click handled below via win<0 */
                    }
                } else {
                    /* desktop: icon hit */
                    int n = desk_icon_count();
                    int hit = -1;
                    for (int i = 0; i < n; i++) {
                        int ix, iy; desk_icon_rect(i, &ix, &iy);
                        int isz = g_desk_isz;
                        if (mx >= ix - 8 && mx < ix + isz + 8 &&
                            my >= iy - 8 && my < iy + isz + 30) {
                            hit = i;
                            break;
                        }
                    }
                    if (hit >= 0) {
                        bool dbl = (g_last_desk_click == hit &&
                                    now - g_last_desk_ms < 500);
                        g_last_desk_ms = now;
                        g_last_desk_click = hit;
                        if (dbl) {
                            if (hit < g_app_count) {
                                gui_open_app_new(g_apps[hit]);
                            } else {
                                /* VFS item: folder -> Files app,
                                 * text file -> Quill with content loaded */
                                const char *icon, *label; const VfsEntry *ve;
                                if (desk_icon_info(hit, &icon, &label, &ve) > 0 && ve) {
                                    if (ve->type == VFS_DIR) {
                                        gui_set_pending_folder(ve->name);
                                        App *f = gui_find_app("Files");
                                        if (f) gui_open_app_new(f);
                                    } else {
                                        /* ".lnk" shortcut -> launch its app */
                                        int nl = (int)strlen(ve->name);
                                        bool lnk = nl >= 5 &&
                                            !strcmp(ve->name + nl - 4, ".lnk");
                                        bool launched = false;
                                        if (lnk) {
                                            char ab[64];
                                            int n = vfs_read(ve->name, ab,
                                                             sizeof(ab) - 1);
                                            if (n > 0) {
                                                if (n > 63) n = 63;
                                                ab[n] = 0;
                                                while (n > 0 && (ab[n-1]=='\n' ||
                                                     ab[n-1]=='\r' || ab[n-1]==' '))
                                                    ab[--n] = 0;
                                                App *a = gui_find_app(ab);
                                                if (a) { gui_open_app_new(a); launched = true; }
                                            }
                                        }
                                        if (!launched) {
                                            /* .txt -> Quill, .png -> Photos,
                                             * anything else -> invalid */
                                            int nl2 = (int)strlen(ve->name);
                                            bool istxt = nl2 > 4 &&
                                                !strcmp(ve->name + nl2 - 4, ".txt");
                                            bool ispng = gui_is_png(ve->name);
                                            if (istxt) {
                                                gui_set_pending_file(ve->name);
                                                App *q = gui_find_app("Quill");
                                                if (q) gui_open_app_new(q);
                                            } else if (ispng) {
                                                gui_set_pending_image("", ve->name);
                                                gui_open_app_new(app_make_photos());
                                            } else if (gui_is_audio(ve->name)) {
                                                gui_launch_audio(ve->name);
                                            } else {
                                                gui_notify("This file is invalid or corrupted!");
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            /* select: keep multi-selection if the icon was
                             * already picked, else select it alone */
                            if (!(g_desk_sel & (1ULL << hit)))
                                g_desk_sel = 1ULL << hit;
                            /* snapshot every icon offset so a drag moves
                             * the whole selected group together */
                            for (int i = 0; i < 64; i++) {
                                int k = desk_key(i);
                                g_drag_base_x[k] = g_icon_dx[k];
                                g_drag_base_y[k] = g_icon_dy[k];
                            }
                            /* arm icon drag: moves after 5px of travel,
                             * otherwise it's just a selection click */
                            g_desk_drag = hit;
                            g_desk_drag_sx = mx; g_desk_drag_sy = my;
                            g_desk_dragging = false;
                        }
                    } else {
                        /* empty desktop: clear selection; drag on real
                         * desktop area starts the rubber-band box */
                        g_desk_sel = 0;
                        g_last_desk_click = -1;
                        if (my < (int)fb_height() - DOCK_H) {
                            g_marquee = 1;
                            g_mar_x0 = g_mar_x1 = mx;
                            g_mar_y0 = g_mar_y1 = my;
                        }
                    }
                    g_mouse_win = -1;
                }
            }
        } else {
            /* mouse up */
            if (g_mouse_win >= 0 && g_mouse_win < g_win_count) {
                GuiEvent ev = {0};
                ev.type = EV_MOUSE_UP; ev.gx = mx; ev.gy = my; ev.button = 0;
                send_event(&g_win[g_mouse_win], &ev);
            }
            g_mouse_win = -1;
            g_drag_win = -1;
            if (g_desk_dragging) {
                /* Win10-style drop: released over a folder icon -> move the
                 * dragged file/folder INTO it (vfs_move reparents) */
                int di = desk_drop_target(mx, my, g_desk_drag);
                if (di >= g_app_count && g_desk_drag >= 0) {
                    const char *icon, *label;
                    const VfsEntry *target;
                    /* desk_icon_info returns a pointer to ONE static
                     * VfsEntry - copy the target's fields NOW, because the
                     * next desk_icon_info call (for the dragged item)
                     * overwrites it. Without this, vfs_move received the
                     * dragged file's own name as the destination and the
                     * drop always failed. */
                    char tname[VFS_NAME_LEN + 1];
                    int tsys = 0, tisdir = 0;
                    if (desk_icon_info(di, &icon, &label, &target) > 0 &&
                        target && target->type == VFS_DIR) {
                        strncpy(tname, target->name, sizeof(tname) - 1);
                        tname[sizeof(tname) - 1] = 0;
                        tsys = target->system;
                        tisdir = 1;
                    }
                    if (tisdir) {
                        /* folder icon center, captured BEFORE the move -
                         * reparenting shifts display indices */
                        int fcx = 0, fcy = 0;
                        desk_icon_rect(di, &fcx, &fcy);
                        fcx += g_desk_isz / 2; fcy += g_desk_isz / 2;
                        if (g_desk_drag < g_app_count) {
                            /* built-in app dropped on a folder -> ".lnk"
                             * shortcut file inside it (Win-style) */
                            if (!tsys) {
                                char nm[VFS_NAME_LEN + 1];
                                snprintf(nm, sizeof(nm), "%s.lnk",
                                         g_apps[g_desk_drag]->name);
                                if (nm[VFS_NAME_LEN]) nm[VFS_NAME_LEN] = 0;
                                if (vfs_create_in(nm, VFS_FILE, tname) != 0 &&
                                    vfs_find_in(tname, nm) < 0)
                                    ; /* give up only if truly can't create */
                                vfs_write_in(tname, nm,
                                             g_apps[g_desk_drag]->name,
                                             (int)strlen(g_apps[g_desk_drag]->name));
                                gui_plop(mx, my, fcx, fcy,
                                         g_apps[g_desk_drag]->name, NULL, NULL);
                            } else gui_deny();
                        } else {
                            const char *icon2, *label2;
                            const VfsEntry *ve;
                            char vname[VFS_NAME_LEN + 1];
                            int vsys = 0, vok = 0;
                            if (desk_icon_info(g_desk_drag, &icon2, &label2,
                                               &ve) > 0 && ve) {
                                strncpy(vname, ve->name, sizeof(vname) - 1);
                                vname[sizeof(vname) - 1] = 0;
                                vsys = ve->system;
                                vok = 1;
                            }
                            if (vok) {
                                if (vsys || tsys)
                                    gui_deny();   /* protected: move denied */
                                else if (vfs_move(vname, NULL, tname) == 0) {
                                    /* keep every icon's pinned position -
                                     * wiping offsets here made folders
                                     * jump back to the grid on each drop */
                                    g_desk_sel = 0;
                                    gui_plop(mx, my, fcx, fcy, icon2,
                                             "", vname);
                                }
                            }
                        }
                    }
                }
                /* released over the dock -> pin the item there (apps and
                 * files/folders alike, like Windows taskbar pinning) */
                if (my > (int)fb_height() - DOCK_H - 44 && g_desk_drag >= 0) {
                    if (g_desk_drag < g_app_count)
                        gui_dock_add_app(g_apps[g_desk_drag]->name);
                    else {
                        const char *ic, *lb; const VfsEntry *ve;
                        if (desk_icon_info(g_desk_drag, &ic, &lb, &ve) > 0 &&
                            ve && !ve->system)
                            gui_dock_add_vfs("", ve->name);
                    }
                }
                settings_save_icons(g_icon_dx, g_icon_dy);  /* persist layout */
            }
            g_desk_drag = -1;
            g_desk_dragging = false;
            /* rubber-band release: final selection (already live-updated) */
            if (g_marquee) {
                g_marquee = 0;
                g_desk_sel = marquee_mask();
            }
            /* dock drag-out release: over a folder -> ".lnk" inside it;
             * over bare desktop -> ".lnk" pinned at the drop point;
             * plain click (no drag) -> normal dock click behavior */
            if (g_dock_press >= 0) {
                int di = g_dock_press;
                if (di >= 0 && di < g_dock_n && g_dock[di].kind == 0 &&
                    g_dock_out) {
                    App *a = g_apps[g_dock[di].app];
                    int fdi = desk_drop_target(mx, my, -1);
                    if (fdi >= g_app_count) {
                        const char *ic, *lb; const VfsEntry *tgt;
                        char tname[VFS_NAME_LEN + 1]; int tok = 0;
                        if (desk_icon_info(fdi, &ic, &lb, &tgt) > 0 &&
                            tgt && tgt->type == VFS_DIR && !tgt->system) {
                            strncpy(tname, tgt->name, sizeof(tname) - 1);
                            tname[sizeof(tname) - 1] = 0;
                            tok = 1;
                        }
                        if (tok) {
                            make_lnk_file(tname, a->name, NULL, 0);
                            int fx = 0, fy = 0;
                            desk_icon_rect(fdi, &fx, &fy);
                            gui_plop(mx, my, fx + g_desk_isz / 2,
                                     fy + g_desk_isz / 2, a->name, NULL, NULL);
                        }
                    } else if (my > MENU_H &&
                               my < (int)fb_height() - DOCK_H &&
                               window_at(mx, my) < 0 &&
                               dock_hit(mx, my) < 0) {
                        char ln[40];
                        if (make_lnk_file(NULL, a->name, ln, sizeof(ln)) == 0)
                            place_icon_at_point(ln, mx, my);
                    }
                } else if (!g_dock_out && di >= 0 && di < g_dock_n) {
                    dock_icon_click(di);
                }
                g_dock_press = -1;
                g_dock_out = false;
            }
            for (int i = 0; i < g_win_count; i++) g_win[i].dragging = false;
        }
    }
    /* middle-click (bit 2): on a dock icon opens ANOTHER instance of that
     * app - quick multitasking like a taskbar/jump-list */
    if (changed & 4) {
        if (btn & 4) {
            int di = dock_hit(mx, my);
            if (di >= 0 && di < g_dock_n && g_dock[di].kind == 0)
                gui_open_app_new(g_apps[g_dock[di].app]);
        }
    }
    /* right-click (bit 1): on an icon -> item menu (Delete for files);
     * on bare desktop -> create menu */
    if (changed & 2) {
        if (btn & 2) {
            int rwin = window_at(mx, my);
            int rdi = dock_hit(mx, my);
            if (rdi >= 0 && rdi < g_dock_n && g_dock[rdi].kind == 0) {
                /* right-click ANY dock app (pinned or not): Close All +
                 * Remove from Dock */
                g_dock_menu = rdi;
                g_dock_menu_app = 1;
                g_dock_menu_x = mx; g_dock_menu_y = my;
                g_ctx_open = 0; g_ctx_icon = -1;
            } else if (rdi >= 0 && rdi < g_dock_n && !g_dock[rdi].pinned &&
                       g_dock[rdi].kind == 1) {
                g_dock_menu = rdi;
                g_dock_menu_app = 0;
                g_dock_menu_x = mx; g_dock_menu_y = my;
                g_ctx_open = 0; g_ctx_icon = -1;
            } else if (my > MENU_H && rwin < 0 && rdi < 0 &&
                my < (int)fb_height() - DOCK_H) {
                int ih = desk_icon_hit(mx, my);
                if (ih >= 0 && ih < g_app_count) {
                    /* built-in app icon: Close All + Add/Remove to Dock */
                    g_ctx_kind = 2;
                    g_ctx_icon = ih;
                    g_ctx_nitems = 2;
                    static const char *a_add[] = { "Close All Windows", "Add to Dock" };
                    static const char *a_rm[]  = { "Close All Windows", "Remove from Dock" };
                    g_ctx_app_items = dock_find_app(ih) >= 0 ? a_rm : a_add;
                } else if (ih >= g_app_count) {
                    /* VFS file/folder: Rename + Delete + Add to Dock
                     * (system items show the menu too; action -> denied) */
                    g_ctx_kind = 1;
                    g_ctx_icon = ih;
                    g_ctx_nitems = 3;
                } else {
                    g_ctx_kind = 0;
                    g_ctx_icon = -1;
                    g_ctx_nitems = 2;
                }
                g_ctx_open = 1;
                g_ctx_x = mx; g_ctx_y = my;
                g_ctx_click_x = mx; g_ctx_click_y = my;
                popup_start_close(3);
            } else if (rwin >= 0 && rwin < g_win_count) {
                /* right-click inside a window -> app gets button=1 event
                 * (Finder uses it for Delete) */
                Window *w = &g_win[rwin];
                if (w->visible && !w->minimized && !w->closing) {
                    focus_window(rwin);
                    GuiEvent ev = {0};
                    ev.type = EV_MOUSE_DOWN; ev.gx = mx; ev.gy = my;
                    ev.x = mx - w->x; ev.y = my - w->y; ev.button = 1;
                    send_event(w, &ev);
                }
                g_ctx_open = 0;
                g_ctx_icon = -1;
            } else {
                g_ctx_open = 0;
                g_ctx_icon = -1;
            }
        }
    }
done_down:
    /* rubber-band: trailing corner follows the pointer; selection updates
     * LIVE while dragging, like Windows */
    if (g_marquee && (g_mbuttons & 1)) {
        g_mar_x1 = mx; g_mar_y1 = my;
        g_desk_sel = marquee_mask();
    }
    /* dock drag-out: past 6px the app icon lifts off the dock */
    if (g_dock_press >= 0 && (g_mbuttons & 1) && !g_dock_out) {
        int ddx = mx - g_dock_press_x, ddy = my - g_dock_press_y;
        if (ddx * ddx + ddy * ddy >= 36) g_dock_out = true;
    }
    /* desktop icon drag: icon follows the pointer once past the threshold.
     * Every selected icon moves by the same delta (group drag). */
    if (g_desk_drag >= 0 && (g_mbuttons & 1)) {
        int dx = mx - g_desk_drag_sx, dy = my - g_desk_drag_sy;
        if (g_desk_dragging || dx * dx + dy * dy >= 25) {
            g_desk_dragging = true;
            for (int i = 0; i < 64; i++) {
                if (!(g_desk_sel & (1ULL << i))) continue;
                int k = desk_key(i);
                g_icon_dx[k] = (int16_t)(g_drag_base_x[k] + dx);
                g_icon_dy[k] = (int16_t)(g_drag_base_y[k] + dy);
            }
        }
    }
    /* window resize drag (bottom-right grip) */
    if (g_resize_win >= 0) {
        if (!(btn & 1) || g_resize_win >= g_win_count) {
            g_resize_win = -1;
        } else {
            Window *w = &g_win[g_resize_win];
            int nw = g_rs_w + (mx - g_rs_x);
            int nh = g_rs_h + (my - g_rs_y);
            /* shrink floor = designed size; can still enlarge to screen */
            if (nw < w->min_w) nw = w->min_w;
            if (nh < w->min_h) nh = w->min_h;
            if (nw > (int)fb_width()) nw = (int)fb_width();
            if (nh > (int)fb_height() - MENU_H) nh = (int)fb_height() - MENU_H;
            w->w = nw; w->h = nh;
        }
    }
    /* dragging update */
    if (g_drag_win >= 0 && g_drag_win < g_win_count) {
        Window *w = &g_win[g_drag_win];
        if (w->dragging) {
            w->x = mx - w->drag_ox;
            w->y = my - w->drag_oy;
            if (w->y < MENU_H) w->y = MENU_H;
            if (w->x < 0) w->x = 0;
            if (w->y + w->h > (int)fb_height() - DOCK_H) w->y = fb_height() - DOCK_H - w->h;
        }
    }

    /* wheel */
    if (g_wheel_accum != 0) {
        /* over the calendar panel -> scroll months (up = earlier) */
        if (g_cal_open &&
            mx >= g_cal_x && mx < g_cal_x + g_cal_w &&
            my >= g_cal_y && my < g_cal_y + g_cal_h) {
            g_cal_moff -= g_wheel_accum;
            if (g_cal_moff < -1200) g_cal_moff = -1200;
            if (g_cal_moff > 1200) g_cal_moff = 1200;
        /* over the instance popup -> scroll its rows (up = positive delta) */
        } else if (g_pop_app >= 0 && mx >= g_pop_x && mx < g_pop_x + g_pop_w &&
            my >= g_pop_y && my < g_pop_y + g_pop_h) {
            g_pop_scroll -= g_wheel_accum * 2;
            if (g_pop_scroll < 0) g_pop_scroll = 0;
        } else {
            int target = (g_mouse_win >= 0) ? g_mouse_win : win;
            if (target < 0 && my > MENU_H && dock_hit(mx, my) < 0) {
                /* over bare desktop -> scroll the icon column */
                g_desk_scroll -= g_wheel_accum * 24;
                if (g_desk_scroll < 0) g_desk_scroll = 0;
            } else if (target >= 0 && target < g_win_count) {
                GuiEvent ev = {0};
                ev.type = EV_MOUSE_WHEEL; ev.gx = mx; ev.gy = my;
                ev.wheel = g_wheel_accum;
                send_event(&g_win[target], &ev);
            }
        }
        g_wheel_accum = 0;
    }

    /* keyboard */
    int c;
    while ((c = kbd_getchar()) >= 0) {
        if (alert_key(c)) continue;
        if (g_rename_active) { rename_key(c); continue; }
        if (g_focus >= 0 && g_focus < g_win_count) {
            Window *w = &g_win[g_focus];
            GuiEvent ev = {0};
            if (c == 0x1B || c >= 0x80) {
                /* specials: driver emits KEY_* values (0x81-0x86) directly,
                 * Esc arrives as raw 0x1B */
                ev.type = EV_KEY_SPECIAL;
                ev.key = (c == 0x1B) ? KEY_ESC : c;
            } else {
                ev.type = EV_KEY_CHAR;
                ev.key = c;
            }
            ev.gx = mx; ev.gy = my;
            send_event(w, &ev);
        }
    }

    g_mx = mx; g_my = my;
    g_mbuttons = btn;
}

/* ---------- cursor ---------- */

/* cursor types come from gui.h (CUR_ARROW..CUR_COUNT) */

struct CursorDef {
    const char *asset;   /* embedded PNG name (cur_*) */
    int16_t hx, hy;      /* hotspot in source pixels */
};

/* Squarebin cursors (daviddarnes/mac-cursors), 32x32 RGBA/LA.
 * Hotspots are the "click point" in source pixels. */
static const CursorDef g_cursors[CUR_COUNT] = {
    [CUR_ARROW]          = { "cur_default",          10,  8 },
    [CUR_IBEAM]          = { "cur_textcursor",       16,  8 },
    [CUR_HAND]           = { "cur_handpointing",     13, 10 },
    [CUR_HAND_OPEN]      = { "cur_handopen",         13, 11 },
    [CUR_HAND_GRAB]      = { "cur_handgrabbing",     13, 11 },
    [CUR_BUSY]           = { "cur_busy",             16, 16 }, /* animated below */
    [CUR_CROSS]          = { "cur_cross",            16, 16 },
    [CUR_MOVE]           = { "cur_move",             16, 16 },
    [CUR_HELP]           = { "cur_help",             15, 17 },
    [CUR_RESIZE_N]       = { "cur_resizenorth",       16, 16 },
    [CUR_RESIZE_S]       = { "cur_resizesouth",       16, 16 },
    [CUR_RESIZE_E]       = { "cur_resizeeast",        16, 16 },
    [CUR_RESIZE_W]       = { "cur_resizewest",        16, 16 },
    [CUR_RESIZE_NE]      = { "cur_resizenortheast",   16, 16 },
    [CUR_RESIZE_NW]      = { "cur_resizenorthwest",   16, 16 },
    [CUR_RESIZE_SE]      = { "cur_resizesoutheast",   16, 16 },
    [CUR_RESIZE_SW]      = { "cur_resizesouthwest",   16, 16 },
    [CUR_RESIZE_LEFTRIGHT]= { "cur_resizeleftright",   16, 16 },
    [CUR_RESIZE_UPDOWN]  = { "cur_resizeupdown",      16, 16 },
    [CUR_ZOOMIN]         = { "cur_zoomin",           16, 16 },
    [CUR_ZOOMOUT]        = { "cur_zoomout",          16, 16 },
    [CUR_CELL]           = { "cur_cell",             16, 16 },
    [CUR_CONTEXTMENU]    = { "cur_contextualmenu",   15, 15 },
    [CUR_NOTALLOWED]     = { "cur_notallowed",       16, 16 },
    [CUR_COPY]           = { "cur_copy",             16, 16 },
};

static int g_cursor = -1;   /* -1 = auto hover-detect; >=0 = forced by app */
static int g_cursor_win = -1; /* window a hover cursor is leased to */

/* apps call this to request a cursor (e.g. CUR_BUSY while loading).
 * Pass -1 to return to automatic hover detection. */
void gui_set_cursor(int type) {
    g_cursor = (type >= CUR_COUNT) ? -1 : type;
    /* hand/ibeam are leased to the window under the pointer: they clear
     * automatically when the mouse leaves, so the arrow returns on the
     * desktop instead of a stale hand sticking around */
    if (type == CUR_HAND || type == CUR_IBEAM)
        g_cursor_win = window_at(g_mx, g_my);
    else
        g_cursor_win = -1;
}

/* pick the cursor for the current hover context when no override is set */
static int cursor_auto(int mx, int my) {
    /* menu-bar widgets (Voyager menu, net, volume, clock) -> hand */
    if (my < MENU_H) {
        if (mx >= g_voy_x0 && mx <= g_voy_x1) return CUR_HAND;
        if (mx >= g_net_x0 && mx <= g_net_x1) return CUR_HAND;
        if (mx >= g_vol_x0 - 22 && mx <= g_vol_x1) return CUR_HAND;
        if (mx >= g_clk_x0 && mx <= g_clk_x1) return CUR_HAND;
    }
    /* over the instance popup -> hand */
    if (g_pop_app >= 0 && mx >= g_pop_x && mx < g_pop_x + g_pop_w &&
        my >= g_pop_y && my < g_pop_y + g_pop_h)
        return CUR_HAND;
    /* over a dock icon -> pointing hand */
    if (dock_hit(mx, my) != -1) return CUR_HAND;
    /* over a window */
    int w = window_at(mx, my);
    if (w >= 0) {
        Window *win = &g_win[w];
        int ty = my - win->y;
        if (ty >= 0 && ty < TITLE_H) {
            /* titlebar: hand cursor over the right-side caption buttons */
            int cx = mx - win->x;
            int cy = TITLE_H / 2;
            int nbtn = win->no_resize ? 2 : 3;
            for (int s = 0; s < nbtn; s++) {
                int pos = win->no_resize ? s + 1 : s;
                int btnx = win->w - 70 + pos * 24;
                int dx = cx - btnx, dy = ty - cy;
                if (dx >= -8 && dx <= 8 && dy >= -8 && dy <= 8) return CUR_HAND;
            }
            return CUR_ARROW;
        }
        /* bottom-right corner: diagonal resize grip */
        int rx = mx - win->x, ry = my - win->y;
        if (!win->maximized && !win->no_resize &&
            rx >= win->w - 18 && ry >= win->h - 14 &&
            ry >= TITLE_H)
            return CUR_RESIZE_SE;
    }
    /* desktop icons stay plain - no hover highlight or hand cursor,
     * matching the pre-highlight desktop look */
    return CUR_ARROW;
}

static void draw_cursor(void) {
    /* expire leased hover cursors the INSTANT the pointer is anywhere
     * the lease doesn't own: another window, the titlebar, the menu
     * bar, the dock, or any floating panel/dialog - so hand/I-beam can
     * never stick over UI it doesn't belong to */
    if ((g_cursor == CUR_HAND || g_cursor == CUR_IBEAM) &&
        g_cursor_win >= 0) {
        int cw = window_at(g_mx, g_my);
        bool leave = (cw != g_cursor_win);
        if (!leave && cw >= 0 && cw < g_win_count &&
            g_my - g_win[cw].y < TITLE_H)
            leave = true;
        /* floating UI drawn above windows also owns the pointer */
        if (!leave && g_mx >= 0) {
            int mx = g_mx, my = g_my;
            if (my < MENU_H || dock_hit(mx, my) >= 0) leave = true;
            else if ((g_cal_open && mx >= g_cal_x && mx < g_cal_x + g_cal_w &&
                      my >= g_cal_y && my < g_cal_y + g_cal_h) ||
                     (g_voy_open && mx >= g_voy_x && mx < g_voy_x + g_voy_w &&
                      my >= g_voy_y && my < g_voy_y + g_voy_h) ||
                     (g_net_open && mx >= g_net_x && mx < g_net_x + g_net_w &&
                      my >= g_net_y && my < g_net_y + g_net_h) ||
                     (g_pop_app >= 0 && mx >= g_pop_x && mx < g_pop_x + g_pop_w &&
                      my >= g_pop_y && my < g_pop_y + g_pop_h) ||
                     (g_ctx_open && mx >= g_ctx_x && mx < g_ctx_x + g_ctx_w &&
                      my >= g_ctx_y && my < g_ctx_y + g_ctx_h) ||
                     (g_alert.active && mx >= g_alert.dx &&
                      mx < g_alert.dx + g_alert.dw &&
                      my >= g_alert.dy && my < g_alert.dy + g_alert.dh) ||
                     (g_rename_active &&
                      mx >= (int)fb_width() / 2 - 140 &&
                      mx < (int)fb_width() / 2 + 140 &&
                      my >= (int)fb_height() / 2 - 46 &&
                      my < (int)fb_height() / 2 + 46))
                leave = true;
            /* save dialog floats over its window too */
            if (!leave && g_save_win >= 0 && g_save_win < g_win_count) {
                int sdx, sdy; save_dlg_rect(&sdx, &sdy);
                if (mx >= sdx && mx < sdx + SAVE_DLG_W &&
                    my >= sdy && my < sdy + SAVE_DLG_H) leave = true;
            }
        }
        if (leave) { g_cursor = -1; g_cursor_win = -1; }
    }
    int type = (g_cursor >= 0) ? g_cursor : cursor_auto(g_mx, g_my);
    /* busy spinner animates ~12fps through rotated beachball frames */
    char asset[40];
    int hx = 16, hy = 16;
    if (type == CUR_BUSY) {
        int frame = (int)((pit_ticks() / 83) % 12);   /* ~12 fps spin */
        snprintf(asset, sizeof(asset), "cur_busyf%02d", frame);
    } else {
        const CursorDef *def = &g_cursors[type];
        snprintf(asset, sizeof(asset), "%s", def->asset);
        hx = def->hx; hy = def->hy;
    }
    uint32_t w = 0, h = 0;
    const uint8_t *img = image_data(asset, &w, &h);
    if (!img) { fb_draw_cursor(g_mx, g_my); return; }  /* fallback arrow */
    /* render at ~40px on screen (was 64px - too big). Hotspot scales with it. */
    const int DISP = 40;
    int dw = DISP, dh = DISP;
    int dhx = (int)(((int64_t)hx * dw) / (int)w);
    int dhy = (int)(((int64_t)hy * dh) / (int)h);
    fb_draw_image_scaled(g_mx - dhx, g_my - dhy, dw, dh, img, (int)w, (int)h);
}

/* ---------- drop "plop" animation ----------
 * when an item lands inside a folder its icon arcs in from the drop point,
 * shrinking as it goes, plus an expanding ring at the target - reads as a
 * little "plop!" insertion. Renders above windows, under the cursor. */
#define PLOP_MAX 8
#define PLOP_MS  420
struct Plop {
    int x0, y0, x1, y1;
    uint64_t start;
    char icon[40];
    const uint8_t *img;   /* optional decoded RGBA (VFS png) */
    uint32_t iw, ih;
    int rev;              /* "taken out": grows, ring at origin */
};
static struct Plop g_plops[PLOP_MAX];
static int g_plop_n;
static int g_plop_rev;   /* 1 = "taken out": grows + ring at origin */

void gui_plop_out(int x0, int y0, int x1, int y1,
                  const char *icon, const char *dir, const char *name) {
    g_plop_rev = 1;
    gui_plop(x0, y0, x1, y1, icon, dir, name);
    g_plop_rev = 0;
}

void gui_plop(int x0, int y0, int x1, int y1,
              const char *icon, const char *dir, const char *name) {
    if (g_plop_n >= PLOP_MAX)
        memmove(g_plops, g_plops + 1, sizeof(g_plops) - sizeof(g_plops[0])),
        g_plop_n = PLOP_MAX - 1;
    struct Plop *p = &g_plops[g_plop_n++];
    memset(p, 0, sizeof(*p));
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->start = pit_ticks();
    p->rev = g_plop_rev;
    if (icon) strncpy(p->icon, icon, sizeof(p->icon) - 1);
    /* a dragged .png flies as its real pixels, not the generic icon */
    if (name) {
        if (gui_is_png(name)) {
            const uint8_t *im = gui_vfs_thumb(dir ? dir : "", name,
                                              &p->iw, &p->ih);
            if (im) p->img = im;
        }
    }
}

static void draw_plops(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < g_plop_n; i++) {
        struct Plop *p = &g_plops[i];
        uint64_t el = now - p->start;
        if (el >= PLOP_MS) {
            memmove(g_plops + i, g_plops + i + 1,
                    (g_plop_n - i - 1) * sizeof(g_plops[0]));
            g_plop_n--; i--; continue;
        }
        int t = (int)(el * 100 / PLOP_MS);          /* 0..100 */
        int e = t * t / 100;                        /* ease-in */
        int px = p->x0 + (p->x1 - p->x0) * e / 100;
        int py = p->y0 + (p->y1 - p->y0) * e / 100;
        /* high arc: the icon sails OVER the folder body and drops onto its
         * contents, like sliding into the folder - not bumping the icon */
        py -= (95 * t * (100 - t)) / 2500;          /* ~95px arc, peaks mid */
        /* plop-in shrinks into the folder; "taken out" grows from it */
        int size = p->rev ? 12 + (36 * e) / 100
                          : 48 - (36 * e) / 100;
        if (size < 10) size = 10;
        const uint8_t *img = p->img;
        uint32_t iw = p->iw, ih = p->ih;
        if (!img) img = asset_image(p->icon[0] ? p->icon : "unknown",
                                    &iw, &ih);
        if (img)
            fb_draw_image_scaled(px - size / 2, py - size / 2, size, size,
                                 img, (int)iw, (int)ih);
        /* expanding ring: at the folder for plop-in, at the origin for
         * "taken out" (where the item came from) */
        int rx = p->rev ? p->x0 : p->x1;
        int ry = p->rev ? p->y0 : p->y1;
        int r = 8 + (30 * t) / 100;
        int a = 150 - (120 * t) / 100;
        uint32_t col = rgba(120, 170, 255, a);
        fb_blend_rect(rx - r, ry - r, 2 * r, 1, col);
        fb_blend_rect(rx - r, ry + r - 1, 2 * r, 1, col);
        fb_blend_rect(rx - r, ry - r, 1, 2 * r, col);
        fb_blend_rect(rx + r - 1, ry - r, 1, 2 * r, col);
    }
}

void gui_drop_app_shortcut(const char *appname, int x, int y) {
    char ln[40];
    if (make_lnk_file(NULL, appname, ln, sizeof(ln)) == 0)
        place_icon_at_point(ln, x, y);
}

/* pin a desktop icon's center exactly at a screen point (used when a file
 * is dragged out of a folder onto the desktop) */
void gui_place_icon_at(const char *name, int x, int y) {
    if (name && name[0]) place_icon_at_point(name, x, y);
}

/* ---------- render ---------- */

static void chime_drain(void);

static void render_frame(void) {
    chime_drain();   /* keep the boot chime streaming without blocking */
    /* destroy windows whose spin-out close animation has finished */
    for (int i = 0; i < g_win_count; i++) {
        Window *w = &g_win[i];
        if (!w->closing || pit_ticks() - w->close_t < 340) continue;
        if (w->snap) { kfree(w->snap); w->snap = NULL; }
        if (w->snap_bands) {
            for (int b = 0; b < w->snap_nb; b++) kfree(w->snap_bands[b]);
            kfree(w->snap_bands); w->snap_bands = NULL;
        }
        if (w->app && w->app->destroy && w->ctx) w->app->destroy(w->ctx);
        for (int j = i; j < g_win_count - 1; j++) g_win[j] = g_win[j + 1];
        g_win_count--; i--;
        if (g_focus >= g_win_count) g_focus = g_win_count - 1;
        if (g_focus >= 0) focus_window(g_focus);
    }
    /* boot splash: while the DA BING plays the desktop is covered by the
     * Voyager startup screen so boot completion is visible */
    if (pit_ticks() < g_splash_until) {
        draw_splash();
        draw_cursor();
        fb_present();
        return;
    }
    draw_wallpaper();
    draw_desktop_icons(g_desk_sel);
    /* rubber-band selection box */
    if (g_marquee) {
        int x0 = g_mar_x0 < g_mar_x1 ? g_mar_x0 : g_mar_x1;
        int y0 = g_mar_y0 < g_mar_y1 ? g_mar_y0 : g_mar_y1;
        int x1 = g_mar_x0 > g_mar_x1 ? g_mar_x0 : g_mar_x1;
        int y1 = g_mar_y0 > g_mar_y1 ? g_mar_y0 : g_mar_y1;
        fb_blend_rect(x0, y0, x1 - x0, y1 - y0, rgba(208, 90, 255, 38));
        fb_blend_rect(x0, y0, x1 - x0, 1, rgba(208, 120, 255, 60));
        fb_blend_rect(x0, y1 - 1, x1 - x0, 1, rgba(208, 120, 255, 60));
        fb_blend_rect(x0, y0, 1, y1 - y0, rgba(208, 120, 255, 60));
        fb_blend_rect(x1 - 1, y0, 1, y1 - y0, rgba(208, 120, 255, 60));
    }
    /* windows bottom-to-top */
    /* draw windows back-to-front by z so clicking raises the app.
     * closing (spin-out) windows are skipped here and drawn LAST so the
     * animation always plays on top of every other window */
    {
        bool drawn[MAX_WINDOWS] = {0};
        for (int pass = 0; pass < g_win_count; pass++) {
            int best = -1;
            for (int i = 0; i < g_win_count; i++)
                if (!drawn[i] && !g_win[i].closing &&
                    (best < 0 || g_win[i].z < g_win[best].z)) best = i;
            if (best < 0) break;
            drawn[best] = true;
            draw_window(best);
        }
        /* spin-outs on top, still roughly z-ordered among themselves */
        for (int pass = 0; pass < g_win_count; pass++) {
            int best = -1;
            for (int i = 0; i < g_win_count; i++)
                if (!drawn[i] && g_win[i].closing &&
                    (best < 0 || g_win[i].z < g_win[best].z)) best = i;
            if (best < 0) break;
            drawn[best] = true;
            draw_window(best);
        }
    }
    draw_menu_bar(g_focus >= 0 && g_focus < g_win_count ? g_win[g_focus].app : NULL);
    draw_dock(g_mx, g_my);
    /* dock drag-out ghost: the app icon floats under the cursor */
    if (g_dock_out && g_dock_press >= 0 && g_dock_press < g_dock_n &&
        g_dock[g_dock_press].kind == 0) {
        App *a = g_apps[g_dock[g_dock_press].app];
        asset_draw((int)g_mx - 24, (int)g_my - 24, 48, a->name);
    }
    draw_instance_popup();
    draw_context_menu();
    draw_deny();
    draw_save_dialog();
    draw_rename_dialog();
    draw_alert();
    draw_plops();
    draw_calendar();
    draw_voyager_menu();
    draw_net_popup();
    /* internet time sync (SNTP, like Windows): once as soon as the link
     * comes up, then every 15 min (retry in 2 min on failure). Fully async
     * - sntp_begin/sntp_poll never block, so it can't collide with app
     * network traffic the way a blocking resolve would */
    {
        uint64_t now = pit_ticks();
        if (!g_sntp_waiting && net_ready() && now >= g_next_sntp &&
            now > g_splash_until) {
            sntp_begin("time.windows.com");
            g_sntp_waiting = 1;
            g_sntp_t0 = now;
        }
        if (g_sntp_waiting) {
            int rc = sntp_poll();
            if (rc == 1) {
                int64_t ntp_unix = (int64_t)sntp_result_secs() - 2208988800LL;
                int64_t off = ntp_unix - rtc_epoch_raw();
                if (off > -86400 && off < 86400) {
                    g_time_offset = off;
                    g_time_synced = 1;
                    kprintf("time: sntp sync ok offset=%d s\n", (int)off);
                }
                g_sntp_done = now;
                g_next_sntp = now + pit_hz() * 900;   /* 15 min */
                g_sntp_waiting = 0;
            } else if (rc < 0 || now - g_sntp_t0 > pit_hz() * 12) {
                g_sntp_fail = now;
                g_next_sntp = now + pit_hz() * 120;   /* retry in 2 min */
                g_sntp_waiting = 0;
                if (rc < 0) kprintf("time: sntp fail\n");
            }
        }
    }
    /* removable-media poll: re-probe the IDE channels every ~2s so USB
     * drives, optical discs and VM-added disks appear/disappear live.
     * Each IDENTIFY is a few port reads - cheap enough at this rate */
    {
        static uint64_t last_media_poll;
        uint64_t now2 = pit_ticks();
        if (now2 - last_media_poll > pit_hz() * 2) {
            last_media_poll = now2;
            ata_poll();
        }
    }
    /* OOM watchdog: warn early, force-kill before the heap can freeze the
     * whole OS. >=85% -> warning alert (re-arms after recovery), >=96% ->
     * close the topmost window (max one per 2s so we never massacre the
     * session in a single frame) */
    {
        size_t used = kmalloc_used(), total = kmalloc_total();
        int pct = total ? (int)(used * 100 / total) : 0;
        static uint64_t last_warn, last_kill;
        uint64_t now = pit_ticks();
        if (pct >= 85 && now - last_warn > pit_hz() * 20) {
            last_warn = now;
            alert_show(ALERT_WARN, "Memory exceeded",
                "Voyager OS is almost out of memory. Close apps or browser "
                "tabs now - the system will close apps automatically.");
        }
        if (pct < 78) last_warn = 0;   /* recovered: re-arm the warning */
        if (pct >= 96 && now - last_kill > pit_hz() * 2) {
            last_kill = now;
            int victim = -1;
            for (int i = 0; i < g_win_count; i++)
                if (g_win[i].visible && !g_win[i].minimized &&
                    !g_win[i].closing &&
                    (victim < 0 || g_win[i].z > g_win[victim].z)) victim = i;
            if (victim >= 0) {
                char an[48] = "?";
                if (g_win[victim].app && g_win[victim].app->name)
                    strncpy(an, g_win[victim].app->name, sizeof(an) - 1);
                an[sizeof(an) - 1] = 0;
                kprintf("oom: force-kill '%s' at %d%% heap\n", an, pct);
                g_force_close = 1;   /* system kill: skip the save veto */
                close_window_idx(victim);
                char amsg[160];
                snprintf(amsg, sizeof(amsg),
                    "Memory exceeded - Voyager OS closed \"%s\" to keep the "
                    "system running. Close apps or browser tabs to free "
                    "memory.", an);
                alert_show(ALERT_ERR, "Memory exceeded", amsg);
            }
        }
    }
    /* low-memory warning: visible over EVERY app so the user knows to
     * close windows/tabs before the heap forces kills. Amber at >82%,
     * pulsing red at >93% */
    {
        size_t used = kmalloc_used(), total = kmalloc_total();
        if (total > 0 && used * 100 / total >= 82) {
            int pct = (int)(used * 100 / total);
            bool crit = pct >= 93;
            uint32_t face = crit ? rgba(140, 20, 20, 235) : rgba(160, 110, 10, 225);
            if (crit && (pit_ticks() / 350) % 2) face = rgba(190, 30, 30, 245);
            int bw = 460, bx = ((int)fb_width() - bw) / 2, by = 34;
            fb_blend_rounded(bx, by, bw, 30, 8, face);
            fb_draw_rect(bx, by, bw, 30, crit ? rgb(255, 120, 120) : rgb(255, 210, 120));
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "Memory exceeded (%d%%) - close apps or tabs now", pct);
            int mw = fb_text_width(msg, 1);
            fb_text(bx + (bw - mw) / 2, by + 10, msg,
                    crit ? rgb(255, 230, 230) : rgb(255, 244, 214), 1);
        }
    }
    draw_cursor();
    fb_present();   /* single atomic flip: no partial-frame flicker */
}

/* draw the Voyager loading screen the instant the framebuffer exists so
 * the user sees "Voyager is starting" during ALL of kernel init instead
 * of the bootloader's leftover frame. gui_boot_chime() later shrinks
 * g_splash_until to cover just the DA BING. */
void gui_early_splash(void) {
    g_splash_start = pit_ticks();
    g_splash_until = g_splash_start + pit_hz() * 30;   /* provisional: whole init */
    render_frame();
}

/* ---------- public ---------- */

/* yield hook for blocking network calls: repaint ~30fps so apps can animate */
static bool net_yield_render(void) {
    static uint64_t last;
    uint64_t now = pit_ticks();
    mouse_poll();          /* keep draining mouse FIFO during blocking net ops */
    if (now - last > pit_hz() / 30) {
        last = now;
        /* pump pointer + repaint so the OS stays responsive while an app is
         * blocked in a synchronous fetch (no app events are delivered) */
        g_net_busy = 1;
        pump_input();
        g_net_busy = 0;
        render_frame();
    }
    return false;
}

void gui_init(void) {
    fb_init();
    /* one-time migration: the old "MICON.png" desktop file becomes
     * "Voyage Player.png" (the music player icon) */
    if (vfs_find("MICON.png") >= 0)
        vfs_rename("MICON.png", "Voyage Player.png");
    else if (vfs_find("micon.png") >= 0)
        vfs_rename("micon.png", "Voyage Player.png");
    net_set_yield_cb(net_yield_render);
    memset(g_images, 0, sizeof(g_images));
    g_app_count = 0;
    g_win_count = 0;
    /* restore persisted desktop icon positions (zeros = default layout) */
    {
        int16_t sdx[64], sdy[64];
        settings_get_icons(sdx, sdy);
        for (int i = 0; i < 64; i++) { g_icon_dx[i] = sdx[i]; g_icon_dy[i] = sdy[i]; }
    }
    wallpaper_build();
    /* Warm the image cache so the first presented frame already has every
     * icon decoded — prevents icons popping in one-by-one (a visible flash)
     * on the first rendered frame. Screen stays on the backdrop meanwhile. */
    static const char *preload[] = {
        "files", "voyage web", "quill", "memo", "calc",
        "console", "settings", "squarebin", "hd"
    };
    for (int i = 0; i < 9; i++) {
        uint32_t w, h;
        asset_image(preload[i], &w, &h);
    }
    /* Preload every cursor PNG too: if a cursor asset ever misses the cache
     * mid-session the renderer falls back to the old software arrow, which
     * looks like the cursor "reverting" during heavy load. */
    for (int i = 0; i < CUR_COUNT; i++) {
        uint32_t w, h;
        asset_image(g_cursors[i].asset, &w, &h);
    }
    for (int i = 0; i < 12; i++) {
        char nm[40];
        snprintf(nm, sizeof(nm), "cur_busyf%02d", i);
        uint32_t w, h;
        asset_image(nm, &w, &h);
    }
}

void gui_main_loop(void) {
    dock_build();   /* apps are registered before the loop starts */
    for (;;) {
        pump_input();
        /* update apps - INCLUDING minimized ones: update() is logic-only
         * (no drawing), and background apps must keep running. Voyage
         * Player relies on this: minimizing it used to starve the DAC
         * and crackle the music. Minimized windows keep w->minimized=1
         * with visible=0, so test BOTH flags - audio must never stop
         * just because its window is tucked away or covered. */
        for (int i = 0; i < g_win_count; i++) {
            Window *w = &g_win[i];
            if ((w->visible || w->minimized) && w->app && w->app->update)
                w->app->update(w->ctx);
        }
        render_frame();
        /* ~60fps cap: matches typical refresh, minimizes present tearing */
        pit_delay_ms(16);
    }
}

/* ---------- Voyager boot chime ("DA BING") ---------- */
/* Real audio: synthesize a stereo PCM arpeggio and stream it through the
 * AC97 driver - actual speakers, not the PC-speaker buzzer. */

#include "../drivers/ac97.h"
#include "../drivers/sine_lut.h"

/* short "deny" alert: two descending square-ish buzzes through the DAC.
 * Appends to the DMA ring (plays right after whatever is queued); skipped
 * if the pipe is nearly full so it never stalls the UI. */
void gui_alert_beep(void) {
    if (!ac97_ready()) return;
    if (ac97_claimed()) return;          /* music owns the DAC - stay silent */
    if (ac97_level() > 120) return;
    const uint32_t rate = ac97_rate() ? ac97_rate() : 48000;
    const uint32_t ms = 170;
    uint32_t nf = rate * ms / 1000;
    int16_t *pcm = (int16_t *)kmalloc((size_t)nf * 4);
    if (!pcm) return;
    /* buzzer-ish timbre: fundamental + 3rd harmonic, two descending notes */
    uint32_t ph = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t f = (i < nf / 2) ? 740 : 494;    /* E5 -> B4 "deny" slide */
        ph = (ph + (uint32_t)((uint64_t)f * 65536 / rate)) & 0xFFFFu;
        int s = SINE_LUT[(ph >> 8) & 255] +
                SINE_LUT[((ph * 3) >> 8) & 255] / 2;
        /* 8ms attack, sustain, 20ms release */
        int env = 1000;
        if (i < rate / 125) env = i * 125 / rate * 8;
        else if (i > nf - rate / 50) env = (nf - i) * 50 / rate * 10;
        int v = s * env / 1000 / 5;
        if (v > 9000) v = 9000; if (v < -9000) v = -9000;
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)v;
    }
    ac97_write(pcm, nf);
    kfree(pcm);
}

/* chime streams through the DMA ring asynchronously: ac97_write blocks
 * until every frame is queued, and 2.6s of 48kHz stereo is ~2.6s of boot
 * stall. Instead we keep the PCM and top up ~21ms per rendered frame. */
static int16_t *g_chime_pcm;
static uint32_t  g_chime_pos, g_chime_total;

static void chime_drain(void) {
    if (!g_chime_pcm) return;
    if (g_chime_pos >= g_chime_total) {
        kfree(g_chime_pcm); g_chime_pcm = NULL; return;
    }
    if (ac97_level() > 200) return;          /* keep ~200ms queued max */
    static int chime_dbg;
    if (chime_dbg++ == 30) ac97_debug();     /* mid-chime hardware state */
    uint32_t chunk = g_chime_total - g_chime_pos;
    if (chunk > 1024) chunk = 1024;          /* ~21ms per frame @48k */
    /* ac97_write is non-blocking now - only advance by what fit */
    g_chime_pos += ac97_write(g_chime_pcm + g_chime_pos * 2, chunk);
}

void gui_boot_chime(void) {
    /* splash covers the ~1s DA BING so boot completion is visible;
     * the bar restarts filling as the chime plays */
    g_splash_start = pit_ticks();
    g_splash_until = g_splash_start + pit_hz() * 11 / 10;
    if (!ac97_ready()) return;
    const uint32_t rate = 48000;
    ac97_set_rate(rate);

    /* Two-note signature: a rounded low "DA" bong, a beat of silence,
     * then a bright stereo "BING" bell - ~1.05s, plays as the desktop
     * finishes loading. SINE_LUT is 256 steps/cycle; phase Q16. */
    const uint32_t fr = rate / 1000;                 /* frames per ms */
    const uint32_t n_da   = 180 * fr;                /* DA   0.00-0.18s */
    const uint32_t n_gap  =  90 * fr;                /* silence 0.18-0.27 */
    const uint32_t n_bing = 680 * fr;                /* BING 0.27-0.96s */
    const uint32_t n_tail =  60 * fr;
    const uint32_t total_frames = n_da + n_gap + n_bing + n_tail;

    int16_t *pcm = (int16_t *)kmalloc((size_t)total_frames * 4);
    if (!pcm) return;
    memset(pcm, 0, (size_t)total_frames * 4);

    uint32_t pos = 0;

    /* DA: pitch-dropping sine bong 240->130Hz, fast attack, smooth
     * decay, slight 2nd harmonic for body - a muted mallet "DA" */
    {
        uint32_t ph = 0, ph2 = 0;
        const uint32_t atk = rate / 200;              /* 5ms attack */
        for (uint32_t i = 0; i < n_da; i++) {
            uint32_t f = 240 - (uint32_t)i * 110 / n_da;
            ph  = (ph  + (uint32_t)((uint64_t)f * 65536 / rate)) & 0xFFFFu;
            ph2 = (ph2 + (uint32_t)((uint64_t)f * 2 * 65536 / rate)) & 0xFFFFu;
            int s = SINE_LUT[(ph >> 8) & 255] +
                    SINE_LUT[(ph2 >> 8) & 255] / 3;
            int env;
            if (i < atk) env = (int)i * 1000 / (int)atk;
            else env = 1000 - (int)((uint64_t)(i - atk) * 1000 /
                                    (n_da - atk));
            if (env < 0) env = 0;
            int amp = s * env / 1000 / 3;
            if (amp > 11000) amp = 11000;
            if (amp < -11000) amp = -11000;
            pcm[pos * 2] = pcm[pos * 2 + 1] = (int16_t)amp;
            pos++;
        }
    }
    pos += n_gap;                                     /* breath before BING */

    /* BING: bright bell - inharmonic partials, stereo spread, slow decay */
    {
        const uint32_t B = 1568;                       /* G6 */
        const uint32_t dp0 = (uint32_t)((uint64_t)B      * 65536 / rate);
        const uint32_t dp1 = (uint32_t)((uint64_t)B*2    * 65536 / rate);
        const uint32_t dp2 = (uint32_t)((uint64_t)B*276/100 * 65536 / rate);
        const uint32_t dp3 = (uint32_t)((uint64_t)B*89/10 * 65536 / rate);
        uint32_t bp[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < n_bing; i++) {
            int e0 = 1000 - i * 700 / n_bing;          /* slow decay   */
            int e1 = 1000 - i * 880 / n_bing;
            int e2 = 1000 - i * 960 / n_bing;          /* fast shimmer  */
            int e3 = 1000 - i * 985 / n_bing;
            if (e0 < 0) e0 = 0; if (e1 < 0) e1 = 0;
            if (e2 < 0) e2 = 0; if (e3 < 0) e3 = 0;
            int att = i < rate / 300 ? i * 300 / (rate / 300) : 300;
            int l = (SINE_LUT[(bp[0] >> 8) & 255] * e0 / 1000 * 2 +
                     SINE_LUT[(bp[2] >> 8) & 255] * e2 / 1000) * att / 300 / 2;
            int r = (SINE_LUT[(bp[1] >> 8) & 255] * e1 / 1000 * 2 +
                     SINE_LUT[(bp[3] >> 8) & 255] * e3 / 1000) * att / 300 / 2;
            if (l > 14000) l = 14000; if (l < -14000) l = -14000;
            if (r > 14000) r = 14000; if (r < -14000) r = -14000;
            pcm[pos * 2] = (int16_t)l;
            pcm[pos * 2 + 1] = (int16_t)r;
            pos++;
            bp[0] = (bp[0] + dp0) & 0xFFFFu;
            bp[1] = (bp[1] + dp1) & 0xFFFFu;
            bp[2] = (bp[2] + dp2) & 0xFFFFu;
            bp[3] = (bp[3] + dp3) & 0xFFFFu;
        }
    }
    pos += n_tail;

    g_chime_pcm = pcm;
    g_chime_pos = 0;
    g_chime_total = total_frames;
    chime_drain();
    /* desktop loop keeps draining via render_frame -> chime_drain */
}
