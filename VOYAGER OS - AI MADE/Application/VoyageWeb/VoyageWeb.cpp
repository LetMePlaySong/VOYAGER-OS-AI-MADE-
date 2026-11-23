/* VoyageWeb.app - Voyage Web: HTML/CSS/JS browser over our TCP/IP stack.
 * Renders HtmlPage display prims (html2.c), dispatches input to the
 * tiny JS engine (js.c), supports links, form widgets, tabs, text
 * selection and caret editing in every text field. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../gui/gui.h"
#include "../../net/net.h"
#include "../../net/http.h"
#include "../../net/html2.h"
#include "../../net/js.h"
#include "../../kernel/fb.h"
#include "../../kernel/vfs.h"

int png_decode(const uint8_t *data, size_t size,
               uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h);

#define HIST_MAX 16
#define TOOLBAR_H 44
#define TAB_H 26
#define STATUS_H 18
#define PAD_L 16
#define PAD_T 8
#define NAV_W 62   /* equal width for < > RELOAD so the toolbar is evenly spaced */
#define REL_W NAV_W

enum { ST_IDLE, ST_LOADING, ST_DONE, ST_ERROR };

#define IMG_MAX 16
#define IMG_MAX_PX 384
/* decoded RGBA budget per tab: 12 MB of pixels. A page that blows past
 * this gets its remaining images dropped + a chrome warning instead of
 * pushing the whole OS heap into the OOM killer. */
#define VOYAGEWEB_IMG_MEM_CAP ((uint64_t)12 << 20)
/* DOM/layout structures budget: warn above ~8 MB estimated */
#define VOYAGEWEB_DOM_MEM_WARN ((size_t)8 << 20)
#define VOYAGEWEB_DOM_MEM_KILL ((size_t)16 << 20)
/* layout-compatible with kernel/gif.c's GifFrame (no shared header) */
typedef struct { uint8_t *rgba; int delay_cs; } VoyageWebGifFrame;
extern "C" int gif_decode_anim(const uint8_t *d, uint32_t n,
                    VoyageWebGifFrame **out_frames, int *out_n,
                    int *ow, int *oh);

typedef struct {
    char *src;              /* raw src attribute (match key), kmalloc'd -
                             * data: URIs can be far larger than a URL */
    char url[512];          /* resolved absolute url */
    uint8_t *rgba;          /* current frame pixels (== frames[0] for GIFs) */
    VoyageWebGifFrame *frames; /* animated GIF frames, NULL for stills */
    int nframes;
    uint64_t anim_t0;       /* gui_ticks() when decoded - animation clock */
    int w, h;
    int dbg_frame;          /* last frame index alpha-histogrammed */
} VoyageWebImg;

typedef struct {
    char url[512];
    char url_edit[512];
    int url_caret;              /* char index of the | cursor */
    int url_sel_a;              /* selection anchor, -1 = none */
    char status[160];
    int state;
    int scroll;
    int last_w;
    int zoom;                   /* page zoom percent, 50..200 */
    uint64_t page_bytes;        /* total bytes fetched for this page
                                 * (html + images + downloads counted) */
    uint64_t img_mem;           /* decoded image pixels held by this tab
                                 * (w*h*4 per frame) - capped so a
                                 * image-heavy page can't OOM the OS */
    int mem_warn;               /* page exceeded memory budget: show the
                                 * "heavy page" notice in the chrome */
    uint64_t last_zoom_t;       /* zoom throttle: full page relayouts are
                                 * heavy - key-repeat zoom storms used to
                                 * stall the GUI loop and starve the DAC,
                                 * making background music "pause" */
    bool url_focused;
    bool need_load;

    HtmlPage *page;

    char hist[HIST_MAX][512];
    int hist_n;
    int hist_pos;               /* index of CURRENT url in hist, -1 none */

    VoyageWebImg imgs[IMG_MAX];
    int nimgs;
    int sbar;                   /* scrollbar thumb grab offset, -1 = none */
} Tab;

typedef struct {
    Tab **tabs;                 /* heap-allocated tabs, grown on demand -
                                 * the "+" button is not artificially capped */
    int ntabs, nalloc;
    int active;

    int hov_ui, prs_ui;         /* toolbar control: 1=back 2=fwd 3=reload 4=url */
    int hov_prim, prs_prim;
    int hov_tab, prs_tab;       /* hovered/pressed tab, -2 = "+", -3 = "<", -4 = ">" */
    int tab_scroll;             /* tab strip horizontal scroll offset (px) */
    int sbar;                   /* scrollbar thumb grab offset, -1 = not dragging */
    int drag_kind;              /* 0 none 1 url-bar 2 page widget */
    int drag_prim;              /* prim idx for widget drag */
    int sel_open;               /* node idx of open <select> dropdown, -1 */
    int sel_x, sel_y, sel_w, sel_h;  /* screen rect of that select widget */
    int sel_hover;              /* hovered dropdown row, -1 */
    int act_x0;                 /* screen x the active-tab highlight
                                 * slid FROM (tab-switch animation) */
    uint64_t act_anim_t;        /* nonzero while the indicator glides */
    uint64_t prs_t;
} VoyageWebCtx;

static void voyageweb_fetch(Tab *t);
static void img_cache_clear(Tab *t);
static Tab *tab(VoyageWebCtx *c) { return c->tabs[c->active]; }

static int tab_metrics(VoyageWebCtx *c, int *tw_out, int *vis_out, int *maxsc_out);

/* screen x of tab i's left edge in the strip (scroll-aware) */
static int tab_x_at(VoyageWebCtx *c, int i) {
    int tw, vis, maxsc;
    tab_metrics(c, &tw, &vis, &maxsc);
    return 4 + i * (tw + 3) - c->tab_scroll;
}
/* switch tabs with the highlight GLIDING from old to new (~200ms) */
static void voyageweb_set_active(VoyageWebCtx *c, int i) {
    if (i == c->active) return;
    if (i < 0 || i >= c->ntabs) return;
    c->act_x0 = tab_x_at(c, c->active);
    c->active = i;
    c->act_anim_t = gui_ticks();
}

static int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return (unsigned char)ca - (unsigned char)cb;
    }
    return 0;
}

/* ---------- lifecycle ---------- */

static void tab_clear(Tab *t) {
    if (t->page) html2_free(t->page);
    t->page = NULL;
    img_cache_clear(t);
    memset(t, 0, sizeof(*t));
    t->url_sel_a = -1;
    t->hist_pos = -1;
    t->sbar = -1;
    t->zoom = 80;               /* default: slightly smaller than 100% so
                                 * body text isn't huge, like browser zoom */
    t->state = ST_IDLE;
    snprintf(t->status, sizeof(t->status), "Ready");
}

/* fresh empty tab, kmalloc'd - callers own the pointer */
static Tab *tab_alloc(void) {
    Tab *t = (Tab *)kzalloc(sizeof(Tab));
    if (!t) return NULL;
    t->url_sel_a = -1;
    t->hist_pos = -1;
    t->sbar = -1;
    t->zoom = 80;
    t->state = ST_IDLE;
    snprintf(t->status, sizeof(t->status), "Ready");
    return t;
}

static void *voyageweb_create(void) {
    VoyageWebCtx *c = (VoyageWebCtx *)kzalloc(sizeof(VoyageWebCtx));
    c->nalloc = 8;
    c->tabs = (Tab **)kzalloc(sizeof(Tab *) * (size_t)c->nalloc);
    c->active = 0;
    c->ntabs = 1;
    c->prs_tab = -1;
    c->sbar = -1;
    c->hov_prim = c->prs_prim = -1;
    c->hov_tab = -1;
    c->sel_open = -1;
    c->sel_hover = -1;
    Tab *t = tab_alloc();
    c->tabs[0] = t;
#ifdef VOYAGER_TESTPAGE
    strcpy(t->url, "http://10.0.2.2:8899/test.html");
#else
    strcpy(t->url, "http://frogfind.com/");
#endif
    strcpy(t->url_edit, t->url);
    t->need_load = true;
    return c;
}

static void voyageweb_destroy(void *ctx) {
    VoyageWebCtx *c = (VoyageWebCtx *)ctx;
    for (int i = 0; i < c->ntabs; i++)
        if (c->tabs[i]) { tab_clear(c->tabs[i]); kfree(c->tabs[i]); }
    if (c->tabs) kfree(c->tabs);
    kfree(c);
}
static void voyageweb_init(void *ctx) { (void)ctx; }

/* ---------- url helpers ---------- */
static bool url_has_scheme(const char *u) { return strstr(u, "://") != NULL; }

static bool url_normalize(const char *in, char *out, size_t cap) {
    char tmp[512];
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *s = tmp;
    while (*s == ' ') s++;
    size_t l = strlen(s);
    while (l && s[l-1] == ' ') s[--l] = 0;
    if (!l) return false;
    const char *rest = s;
    if (!strncmp(s, "https://", 8)) rest = s + 8;       /* https -> http (no TLS) */
    else if (!strncmp(s, "http://", 7)) rest = s + 7;
    else if (strchr(s, ':')) return false;             /* other scheme: unsupported */
    char host[256];
    const char *path = "/";
    /* path may be absent but ?query / #frag can still follow the host -
     * they must not get swallowed into the hostname */
    const char *end = rest + strlen(rest);
    const char *slash = strchr(rest, '/');
    const char *q = strchr(rest, '?');
    const char *fr = strchr(rest, '#');
    const char *cut = end;
    if (slash && slash < cut) cut = slash;
    if (q && q < cut) cut = q;
    if (fr && fr < cut) cut = fr;
    size_t hl = cut - rest;
    if (hl > 250) hl = 250;
    memcpy(host, rest, hl); host[hl] = 0;
    path = cut < end ? cut : "/";
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    /* not a domain (no dot) -> Voyager search through frogfind instead
     * of a dead "cannot connect": http://frogfind.com/?q=<query> */
    if (!strchr(host, '.') && host[0]) {
        int n = snprintf(out, cap, "http://frogfind.com/?q=");
        if (n < 0 || n >= (int)cap) return false;
        int qi = n;
        for (const char *q = s; *q && qi < (int)cap - 4; q++) {
            unsigned char ch = (unsigned char)*q;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                ch == '.' || ch == '~') {
                out[qi++] = (char)ch;
            } else {
                static const char *hx = "0123456789ABCDEF";
                out[qi++] = '%';
                out[qi++] = hx[ch >> 4];
                out[qi++] = hx[ch & 15];
            }
        }
        out[qi] = 0;
        return true;
    }
    char hp[300];
    if (path[0] == '?' || path[0] == '#')
        snprintf(hp, sizeof(hp), "http://%s/%s", host, path);
    else
        snprintf(hp, sizeof(hp), "http://%s%s", host, path);
    int n = snprintf(out, cap, "%s", hp);
    return n > 0 && n < (int)cap;
}

static void url_split(const char *url, char *host, size_t hc,
                      const char **path) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t hl = slash - p;
        if (hl >= hc) hl = hc - 1;
        memcpy(host, p, hl); host[hl] = 0;
        *path = slash;
    } else {
        strncpy(host, p, hc - 1); host[hc - 1] = 0;
        *path = "/";
    }
}

/* resolve a (possibly relative) url against a base page url */
/* collapse /./ and /../ segments in the path portion of an http URL */
static void url_resolve_dotdot(const char *in, char *out, size_t cap) {
    static char segs[96][160];
    const char *path_start;
    char prefix[320]; prefix[0] = 0;
    const char *proto = strstr(in, "://");
    if (proto) {
        const char *slash = strchr(proto + 3, '/');
        if (!slash) { strncpy(out, in, cap - 1); out[cap - 1] = 0; return; }
        int pl = (int)(slash - in);
        if (pl > 300) pl = 300;
        memcpy(prefix, in, pl); prefix[pl] = 0;
        path_start = slash;
    } else {
        path_start = in;
        if (*path_start == '/') path_start++;
    }
    int ns = 0;
    const char *s = path_start;
    while (*s && ns < 90) {
        while (*s == '/') s++;
        const char *e = s;
        while (*e && *e != '/') e++;
        int l = (int)(e - s);
        if (l <= 0) break;
        if (!(l == 1 && s[0] == '.')) {
            if (l == 2 && s[0] == '.' && s[1] == '.') { if (ns > 0) ns--; }
            else {
                if (l > 150) l = 150;
                memcpy(segs[ns], s, l); segs[ns][l] = 0; ns++;
            }
        }
        s = e;
    }
    int o = 0;
    o += snprintf(out + o, cap - o, "%s", prefix);
    for (int i = 0; i < ns && o < (int)cap - 170; i++)
        o += snprintf(out + o, cap - o, "/%s", segs[i]);
    if (o >= (int)cap) out[cap - 1] = 0;
}

static void url_resolve_base(const char *base, const char *href,
                             char *out, size_t cap) {
    out[0] = 0;
    if (!href || !href[0]) return;
    if (!strncasecmp(href, "javascript:", 11) || href[0] == '#') return;
    char raw[700];
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        if (!strncmp(href, "https://", 8))
            snprintf(raw, sizeof raw, "http://%s", href + 8);
        else
            snprintf(raw, sizeof raw, "%s", href);
    } else if (href[0] == '/' && href[1] == '/') {
        snprintf(raw, sizeof raw, "http:%s", href);   /* protocol-relative //host/x */
    } else {
        char host[256]; const char *path;
        url_split(base, host, sizeof(host), &path);
        if (href[0] == '/') {
            snprintf(raw, sizeof raw, "http://%s%s", host, href);
        } else if (href[0] == '?') {
            /* query-only: keep the base path, swap the query */
            char b[512];
            strncpy(b, base, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
            char *qm = strchr(b, '?'); if (qm) *qm = 0;
            char *fr = strchr(b, '#'); if (fr) *fr = 0;
            snprintf(raw, sizeof raw, "%s%s", b, href);
        } else {
            char b[512];
            strncpy(b, base, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
            char *sl = strrchr(b, '/');
            if (sl) sl[1] = 0; else b[0] = 0;
            snprintf(raw, sizeof raw, "%s%s", b, href);
        }
    }
    url_resolve_dotdot(raw, out, cap);
}

/* ---------- history (per tab) ---------- */
static void hist_add(Tab *t, const char *url) {
    if (t->hist_pos < t->hist_n - 1) t->hist_n = t->hist_pos + 1; /* drop fwd */
    if (t->hist_n >= HIST_MAX) {
        memmove(t->hist[0], t->hist[1], sizeof(t->hist) - sizeof(t->hist[0]));
        t->hist_n--;
        if (t->hist_pos > 0) t->hist_pos--;
    }
    strncpy(t->hist[t->hist_n++], url, 511);
    t->hist[t->hist_n-1][511] = 0;
    t->hist_pos = t->hist_n - 1;
}

static void tab_nav(Tab *t, const char *url) {
    if (!url || !url[0]) return;
    hist_add(t, url);
    strncpy(t->url, url, 511); t->url[511] = 0;
    strncpy(t->url_edit, url, 511); t->url_edit[511] = 0;
    t->url_caret = (int)strlen(t->url_edit);
    t->url_sel_a = -1;
    t->scroll = 0;          /* new page always starts at the top - keeping
                             * the old offset made fresh loads look wiped,
                             * showing only middle fragments of the page */
    voyageweb_fetch(t);
}

static void navigate(VoyageWebCtx *c, const char *url) { tab_nav(tab(c), url); }

static void tab_go_back(Tab *t) {
    if (t->hist_pos <= 0) return;
    t->hist_pos--;
    strncpy(t->url, t->hist[t->hist_pos], 511); t->url[511] = 0;
    strncpy(t->url_edit, t->url, 511); t->url_edit[511] = 0;
    t->url_caret = (int)strlen(t->url_edit);
    t->url_sel_a = -1;
    t->scroll = 0;
    voyageweb_fetch(t);
}
static void tab_go_fwd(Tab *t) {
    if (t->hist_pos >= t->hist_n - 1) return;
    t->hist_pos++;
    strncpy(t->url, t->hist[t->hist_pos], 511); t->url[511] = 0;
    strncpy(t->url_edit, t->url, 511); t->url_edit[511] = 0;
    t->url_caret = (int)strlen(t->url_edit);
    t->url_sel_a = -1;
    t->scroll = 0;
    voyageweb_fetch(t);
}

/* ---------- page images ---------- */
static void img_cache_clear(Tab *t) {
    for (int i = 0; i < IMG_MAX; i++) {
        if (t->imgs[i].frames) {
            for (int f = 0; f < t->imgs[i].nframes; f++)
                if (t->imgs[i].frames[f].rgba) kfree(t->imgs[i].frames[f].rgba);
            kfree(t->imgs[i].frames);
        } else if (t->imgs[i].rgba) {
            kfree(t->imgs[i].rgba);
        }
        if (t->imgs[i].src) kfree(t->imgs[i].src);
        memset(&t->imgs[i], 0, sizeof(t->imgs[i]));
    }
    t->nimgs = 0;
    t->img_mem = 0;
}

static VoyageWebImg *img_slot(Tab *t) {
    for (int i = 0; i < t->nimgs; i++)
        if (!t->imgs[i].rgba && !t->imgs[i].src) return &t->imgs[i];
    if (t->nimgs < IMG_MAX) return &t->imgs[t->nimgs++];
    return NULL;
}

static VoyageWebImg *img_find_src(Tab *t, const char *src) {
    for (int i = 0; i < t->nimgs; i++)
        if (t->imgs[i].src && !strcmp(t->imgs[i].src, src)) return &t->imgs[i];
    return NULL;
}

/* uncompressed BMP (24/32bpp) -> RGBA */
static int bmp_decode(const uint8_t *d, uint32_t n,
                      uint8_t **out, int *ow, int *oh) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off = d[10] | (d[11] << 8) | (d[12] << 16) | ((uint32_t)d[13] << 24);
    int32_t w = (int32_t)(d[18] | (d[19] << 8) | (d[20] << 16) | ((uint32_t)d[21] << 24));
    int32_t h = (int32_t)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24));
    uint16_t bpp = d[28] | (d[29] << 8);
    uint32_t comp = d[30] | (d[31] << 8) | (d[32] << 16) | ((uint32_t)d[33] << 24);
    if (comp != 0 || (bpp != 24 && bpp != 32)) return -1;
    int topdown = h < 0; if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return -1;
    int bytes = bpp / 8;
    uint32_t stride = ((uint32_t)w * bytes + 3) & ~3u;
    if (off + (uint32_t)h * stride > n) return -1;
    /* 32-bit BMP alpha is unreliable: most encoders leave the byte 0,
     * which would make the whole image "transparent" (invisible). If
     * ~all alpha bytes are 0 the channel is dead - treat as opaque. */
    int dead_alpha = 1;
    if (bytes == 4) {
        int nonzero = 0, total = w * h;
        for (int y = 0; y < h && nonzero * 10 < total; y++) {
            const uint8_t *s = d + off + (uint32_t)(topdown ? y : h - 1 - y) * stride;
            for (int x = 0; x < w; x++)
                if (s[x * 4 + 3]) { nonzero++; break; }
        }
        dead_alpha = (nonzero == 0);
    }
    uint8_t *rgba = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!rgba) return -1;
    for (int y = 0; y < h; y++) {
        int sy = topdown ? y : h - 1 - y;
        const uint8_t *s = d + off + (uint32_t)sy * stride;
        uint8_t *o = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = s + x * bytes;
            o[x*4+0] = p[2]; o[x*4+1] = p[1]; o[x*4+2] = p[0];
            o[x*4+3] = (bytes == 4 && !dead_alpha) ? p[3] : 255;
        }
    }
    *out = rgba; *ow = w; *oh = h;
    return 0;
}

extern "C" int gif_decode(const uint8_t *d, uint32_t n,
               uint8_t **out, int *ow, int *oh);
extern "C" int jpg_decode(const uint8_t *d, uint32_t n,
               uint8_t **out, int *ow, int *oh);

/* scale a decoded RGBA buffer down so neither dim exceeds maxdim.
 * Images larger than IMG_MAX_PX used to be REJECTED entirely -> the
 * page showed the opaque gray placeholder, which is why "transparent"
 * photos/logos looked like they had no alpha at all. */
static int rgba_scale_down(uint8_t *src, int sw, int sh,
                           uint8_t **out, int *ow, int *oh, int maxdim) {
    int nw = sw, nh = sh;
    if (nw > maxdim) { nh = nh * maxdim / nw; nw = maxdim; }
    if (nh > maxdim) { nw = nw * maxdim / nh; nh = maxdim; }
    if (nw <= 0) nw = 1; if (nh <= 0) nh = 1;
    uint8_t *d = (uint8_t *)kmalloc((size_t)nw * nh * 4);
    if (!d) return -1;
    for (int y = 0; y < nh; y++) {
        int sy = y * sh / nh;
        for (int x = 0; x < nw; x++) {
            int sx = x * sw / nw;
            memcpy(d + ((size_t)y * nw + x) * 4,
                   src + ((size_t)sy * sw + sx) * 4, 4);
        }
    }
    kfree(src);
    *out = d; *ow = nw; *oh = nh;
    return 0;
}

static int img_decode(const uint8_t *d, uint32_t n, uint8_t **out, int *w, int *h,
                      VoyageWebGifFrame **frames_out, int *nframes_out) {
    uint8_t *r = NULL; int rw = 0, rh = 0; int rc;
    VoyageWebGifFrame *gframes = NULL; int gnfr = 0;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        uint32_t pw2 = 0, ph2 = 0;
        __asm__ volatile("cli");
        rc = png_decode(d, n, &r, &pw2, &ph2);
        __asm__ volatile("sti");
        rw = (int)pw2; rh = (int)ph2;
    } else {
        __asm__ volatile("cli");
        if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') {
            /* animated path: every frame becomes a full-canvas snapshot;
             * gif_decode (frame 0 only) remains for callers that can't
             * animate, but VoyageWeb always wants the frames */
            rc = gif_decode_anim(d, n, &gframes, &gnfr, &rw, &rh);
            if (rc == 0 && gnfr > 0) r = gframes[0].rgba;
            else { rc = -1; }
        } else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8)
            rc = jpg_decode(d, n, &r, &rw, &rh);
        else
            rc = bmp_decode(d, n, &r, &rw, &rh);
        __asm__ volatile("sti");
    }
    if (rc || !r || rw <= 0 || rh <= 0 || rw > 4096 || rh > 4096) {
        if (gframes) {
            for (int i = 0; i < gnfr; i++)
                if (gframes[i].rgba) kfree(gframes[i].rgba);
            kfree(gframes);
        } else if (r) kfree(r);
        return -1;
    }
    /* oversized: downscale instead of dropping (keeps alpha intact) */
    if (rw > IMG_MAX_PX || rh > IMG_MAX_PX) {
        if (gframes) {
            for (int i = 0; i < gnfr; i++) {
                uint8_t *s2 = NULL; int w2 = 0, h2 = 0;
                if (gframes[i].rgba &&
                    rgba_scale_down(gframes[i].rgba, rw, rh,
                                    &s2, &w2, &h2, IMG_MAX_PX) == 0) {
                    gframes[i].rgba = s2;
                }
            }
            /* recompute the shared canvas dims once (all frames scale
             * identically); a frame whose scale alloc failed keeps its
             * original buffer - blit samples it slightly stretched but
             * stays in bounds since src dims only shrink */
            int nw = rw, nh = rh;
            if (nw > IMG_MAX_PX) { nh = nh * IMG_MAX_PX / nw; nw = IMG_MAX_PX; }
            if (nh > IMG_MAX_PX) { nw = nw * IMG_MAX_PX / nh; nh = IMG_MAX_PX; }
            rw = nw; rh = nh;
            r = gframes[0].rgba;
        } else {
            uint8_t *s2 = NULL; int w2 = 0, h2 = 0;
            if (rgba_scale_down(r, rw, rh, &s2, &w2, &h2, IMG_MAX_PX) == 0) {
                r = s2; rw = w2; rh = h2;
            }
            /* if the scale alloc failed keep the original - blit scales */
        }
    }
    /* diagnostic: alpha histogram of the decoded buffer - settles
     * decoder-vs-blitter questions from the serial log alone */
    {
        long z = 0, op = 0, pa = 0, tot = (long)rw * rh;
        if (tot > 262144) tot = 262144;   /* sample cap: keeps decode fast */
        const uint8_t *scan = r;
        for (long i = 0; i < tot; i++) {
            uint32_t a = scan[i * 4 + 3];
            if (a == 0) z++; else if (a == 255) op++; else pa++;
        }
        kprintf("img alpha %dx%d: zero=%d opaque=%d partial=%d\n",
                rw, rh, (int)z, (int)op, (int)pa);
    }
    *out = r; *w = rw; *h = rh;
    if (frames_out) *frames_out = gframes;
    if (nframes_out) *nframes_out = gnfr;
    return 0;
}

/* pull src="..." out of an <img ...> tag; returns length or 0 */
static int img_src_of(const char *tag, int tlen, char *out, int cap) {
    for (int i = 0; i + 3 < tlen; i++) {
        if ((tag[i] == 's' || tag[i] == 'S') &&
            (tag[i+1] == 'r' || tag[i+1] == 'R') &&
            (tag[i+2] == 'c' || tag[i+2] == 'C')) {
            if (i > 0) {
                char pc = tag[i-1];
                if ((pc>='a'&&pc<='z')||(pc>='A'&&pc<='Z')||(pc>='0'&&pc<='9')||
                    pc=='-'||pc=='_') continue;
            }
            int j = i + 3;
            if (j < tlen && !((tag[j]==' ')||(tag[j]=='\t')||tag[j]=='='))
                continue;
            while (j < tlen && (tag[j] == ' ' || tag[j] == '\t' || tag[j] == '=')) j++;
            if (j >= tlen) return 0;
            char q = tag[j];
            int k = (q == '"' || q == '\'') ? j + 1 : j, o = 0;
            if (q == '"' || q == '\'') {
                while (k < tlen && tag[k] != q && o < cap - 1) out[o++] = tag[k++];
            } else {
                while (k < tlen && tag[k] != ' ' && tag[k] != '>' && o < cap - 1)
                    out[o++] = tag[k++];
            }
            out[o] = 0;
            return o;
        }
    }
    return 0;
}

/* the HTML parser entity-decodes attribute values, so n->src holds the
 * DECODED url while our raw scan sees &amp; etc. Decode here so the cache
 * key matches what html2's img_find() compares against. */
static int html_ent_decode(const char *in, char *out, int cap) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; i++) {
        if (in[i] == '&') {
            if (!strncmp(in + i, "&amp;", 5)) { out[o++] = '&'; i += 4; continue; }
            if (!strncmp(in + i, "&lt;", 4))  { out[o++] = '<'; i += 3; continue; }
            if (!strncmp(in + i, "&gt;", 4))  { out[o++] = '>'; i += 3; continue; }
            if (!strncmp(in + i, "&quot;", 6)){ out[o++] = '"'; i += 5; continue; }
            if (!strncmp(in + i, "&#39;", 5) || !strncmp(in + i, "&apos;", 6)) {
                out[o++] = '\''; i += in[i+2] == '3' ? 4 : 5; continue;
            }
            if (in[i+1] == '#') {
                int v = 0, j = i + 2, base = 10;
                if (in[j] == 'x' || in[j] == 'X') { base = 16; j++; }
                while (in[j] >= '0' && in[j] <= '9' ||
                       base == 16 && in[j] >= 'a' && in[j] <= 'f' ||
                       base == 16 && in[j] >= 'A' && in[j] <= 'F') {
                    int d = in[j] <= '9' ? in[j] - '0' :
                            (in[j] | 32) - 'a' + 10;
                    v = v * base + d; j++;
                }
                if (j > i + 2 && in[j] == ';' && v > 0 && v < 128) {
                    out[o++] = (char)v; i = j; continue;
                }
            }
        }
        out[o++] = in[i];
    }
    out[o] = 0;
    return o;
}

/* base64 decode: returns kmalloc'd bytes, *outn = length, or NULL */
static uint8_t *b64_decode(const char *in, size_t n, size_t *outn) {
    int tbl[256];
    for (int i = 0; i < 256; i++) tbl[i] = -1;
    const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) tbl[(uint8_t)a[i]] = i;
    uint8_t *out = (uint8_t *)kmalloc(n / 4 * 3 + 8);
    if (!out) return NULL;
    size_t o = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = tbl[(uint8_t)in[i]];
        if (v < 0) continue;              /* skip whitespace/junk */
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *outn = o;
    return out;
}

/* decode a data: URI payload -> raw image bytes (caller kfree's).
 * handles ;base64 and percent-encoded forms. */
static uint8_t *data_uri_decode(const char *src, size_t n, size_t *outn) {
    const char *comma = NULL;
    for (size_t i = 0; i < n; i++)
        if (src[i] == ',') { comma = src + i; break; }
    if (!comma) return NULL;
    const char *meta = src + 5;               /* after "data:" */
    size_t ml = (size_t)(comma - meta);
    int b64 = 0;
    /* ";base64" must appear at the end of the metadata */
    if (ml >= 7 && !strncasecmp(comma - 7, ";base64", 7)) b64 = 1;
    const char *pay = comma + 1;
    size_t pn = n - (size_t)(pay - src);
    if (b64) return b64_decode(pay, pn, outn);
    /* non-base64: percent-decode */
    uint8_t *out = (uint8_t *)kmalloc(pn + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < pn; i++) {
        if (pay[i] == '%' && i + 2 < pn) {
            int h1 = pay[i+1], h2 = pay[i+2];
            int v1 = h1 <= '9' ? h1 - '0' : (h1 | 32) - 'a' + 10;
            int v2 = h2 <= '9' ? h2 - '0' : (h2 | 32) - 'a' + 10;
            if (v1 >= 0 && v1 < 16 && v2 >= 0 && v2 < 16) {
                out[o++] = (uint8_t)(v1 * 16 + v2); i += 2; continue;
            }
        }
        out[o++] = (uint8_t)pay[i];
    }
    *outn = o;
    return out;
}

static void load_page_images(Tab *t, const char *html, size_t len) {
    img_cache_clear(t);
    if (!html || !len) return;
    const char *p = html, *end = html + len;
    while (p + 4 < end && t->nimgs < IMG_MAX) {
        const char *q = NULL;
        for (const char *s = p; s + 4 <= end; s++) {
            if (s[0] == '<' && (s[1] == 'i' || s[1] == 'I') &&
                (s[2] == 'm' || s[2] == 'M') && (s[3] == 'g' || s[3] == 'G') &&
                (s[4] == ' ' || s[4] == '\t' || s[4] == '\n' || s[4] == '\r' ||
                 s[4] == '\f' || s[4] == '/' || s[4] == '>')) { q = s; break; }
        }
        if (!q) break;
        const char *gt = q;
        while (gt < end && *gt != '>') gt++;
        if (gt >= end) break;
        int tlen = (int)(gt - q);
        /* src can be a huge data: URI - extract into a heap buffer sized
         * to the tag instead of truncating at 384 bytes (which silently
         * killed every embedded image) */
        char *raw = (char *)kmalloc((size_t)tlen + 1);
        char *src = NULL;
        if (!raw) { p = gt + 1; continue; }
        int rl = img_src_of(q, tlen, raw, tlen + 1);
        if (rl <= 0 || !raw[0]) { kfree(raw); p = gt + 1; continue; }
        /* decoded src is what html2 stores in n->src and img_find()
         * compares - entity-decode into a heap buffer too */
        src = (char *)kmalloc((size_t)rl + 1);
        if (src) html_ent_decode(raw, src, rl + 1); else src = raw, raw = NULL;
        kfree(raw);
        if (!src || !src[0] || img_find_src(t, src)) {
            if (src && src != raw) kfree(src);
            p = gt + 1; continue;
        }
        {
            VoyageWebImg *im = img_slot(t);
            if (im) {
                /* per-tab decoded-pixel budget: a page stuffed with huge
                 * images must not be able to exhaust the system heap.
                 * Over-budget images keep their src (broken-image look)
                 * and flip mem_warn so the chrome can say why. */
                if (t->img_mem >= VOYAGEWEB_IMG_MEM_CAP) {
                    t->mem_warn = 1;
                    kprintf("img: mem cap hit, skipping %s\n", src);
                    if (src) kfree(src);
                    p = gt + 1;
                    continue;
                }
                im->src = src; src = NULL;   /* cache owns the key now */
                uint8_t *bytes = NULL; size_t bn = 0;
                if (!strncasecmp(im->src, "data:", 5) &&
                    strlen(im->src) < (512u << 10)) {
                    /* embedded image: decode inline, no network needed */
                    bytes = data_uri_decode(im->src, strlen(im->src), &bn);
                    t->page_bytes += bn;
                    strncpy(im->url, "data:", sizeof(im->url) - 1);
                } else {
                    char abs[512];
                    url_resolve_base(t->url, im->src, abs, sizeof(abs));
                    strncpy(im->url, abs, sizeof(im->url) - 1);
                    im->url[sizeof(im->url)-1] = 0;
                    if (im->url[0] && !strncmp(im->url, "http://", 7) &&
                        net_ready()) {
                        char host[256]; const char *path;
                        url_split(im->url, host, sizeof(host), &path);
                        HttpResp r;
                        int hg = http_get(host, 80, path, &r, 8000);
                        /* no content-type gate: servers label images all
                         * sorts of ways (octet-stream, binary, missing) -
                         * img_decode magic-checks the bytes itself and
                         * rejects anything that isn't a real image */
                        if (hg == 0 && r.status < 400 && r.body &&
                            r.body_len > 0 && r.body_len < (4u << 20)) {
                            t->page_bytes += r.body_len;   /* count image bytes */
                            bytes = (uint8_t *)r.body;
                            bn = r.body_len;
                            /* keep r alive: bytes aliases r.body */
                            uint8_t *rgba = NULL; int w = 0, h = 0;
                            VoyageWebGifFrame *fr = NULL; int nfr = 0;
                            if (img_decode(bytes, (uint32_t)bn,
                                           &rgba, &w, &h, &fr, &nfr) == 0) {
                                uint64_t need = (uint64_t)w * h * 4 *
                                                (nfr > 1 ? nfr : 1);
                                if (t->img_mem + need > VOYAGEWEB_IMG_MEM_CAP) {
                                    /* decoded fine but busts the budget:
                                     * drop it rather than stockpile */
                                    for (int f = 0; f < nfr; f++)
                                        if (fr[f].rgba) kfree(fr[f].rgba);
                                    if (fr) kfree(fr);
                                    if (!nfr && rgba) kfree(rgba);
                                    t->mem_warn = 1;
                                    kprintf("img: over budget %s (%ux%u x%d)\n",
                                            im->src, w, h, nfr);
                                } else {
                                    im->rgba = rgba; im->w = w; im->h = h; im->dbg_frame = -1;
                                    im->frames = fr; im->nframes = nfr;
                                    im->anim_t0 = gui_ticks();
                                    t->img_mem += need;
                                    kprintf("img: %s -> %dx%d%s\n", im->src, w, h,
                                            nfr > 1 ? " anim" : "");
                                }
                            } else {
                                kprintf("img: decode fail %s (%uB %s)\n",
                                        im->src, (unsigned)bn,
                                        r.content_type);
                            }
                            http_free(&r);
                            bytes = NULL;
                        } else {
                            kprintf("img: fetch fail %s hg=%d st=%d\n",
                                    im->src, hg,
                                    hg == 0 ? r.status : -1);
                            if (hg == 0) http_free(&r);
                        }
                    } else {
                        kprintf("img: skip %s -> %s\n", im->src, im->url);
                    }
                }
                if (bytes) {   /* data: URI path */
                    uint8_t *rgba = NULL; int w = 0, h = 0;
                    VoyageWebGifFrame *fr = NULL; int nfr = 0;
                    if (img_decode(bytes, (uint32_t)bn,
                                   &rgba, &w, &h, &fr, &nfr) == 0) {
                        uint64_t need = (uint64_t)w * h * 4 *
                                        (nfr > 1 ? nfr : 1);
                        if (t->img_mem + need > VOYAGEWEB_IMG_MEM_CAP) {
                            for (int f = 0; f < nfr; f++)
                                if (fr[f].rgba) kfree(fr[f].rgba);
                            if (fr) kfree(fr);
                            if (!nfr && rgba) kfree(rgba);
                            t->mem_warn = 1;
                            kprintf("img: data-uri over budget %ux%u x%d\n",
                                    w, h, nfr);
                        } else {
                            im->rgba = rgba; im->w = w; im->h = h; im->dbg_frame = -1;
                            im->frames = fr; im->nframes = nfr;
                            im->anim_t0 = gui_ticks();
                            t->img_mem += need;
                            kprintf("img: data-uri -> %dx%d%s\n", w, h,
                                    nfr > 1 ? " anim" : "");
                        }
                    } else {
                        kprintf("img: data-uri decode fail\n");
                    }
                    kfree(bytes);
                }
            }
        }
        if (src) kfree(src);
        p = gt + 1;
    }
}

/* ---------- fetch ---------- */
/* find a header value (case-insensitive name) in a raw header block */
static const char *http_header(const HttpResp *r, const char *name) {
    if (!r->header_block || !r->header_len) return NULL;
    static char val[400];
    size_t nl = strlen(name);
    const char *p = r->header_block;
    const char *end = p + r->header_len;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\r' && *eol != '\n') eol++;
        if ((size_t)(eol - p) > nl + 1 && p[nl] == ':') {
            int match = 1;
            for (size_t i = 0; i < nl; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = 0; break; }
            }
            if (match) {
                const char *v = p + nl + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(eol - v);
                if (vl >= sizeof val) vl = sizeof val - 1;
                memcpy(val, v, vl); val[vl] = 0;
                return val;
            }
        }
        p = eol;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
    }
    return NULL;
}

/* save a fetched file into the VFS "Downloads" folder. Returns 1 on
 * success. Filenames are sanitized and deduped; VFS names cap at 24
 * chars so the extension is preserved by truncating the stem. */
static char g_dl_dir[24] = "Downloads";   /* folder downloads land in */
static int voyageweb_save_download(const char *path, const char *body,
                                uint32_t blen, char *outname, int outcap) {
    if (!body || !blen || blen > (8u << 20)) return -1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0]) base = "download";
    /* split extension so truncation keeps it */
    char stem[40], ext[12];
    stem[0] = ext[0] = 0;
    const char *dot = strrchr(base, '.');
    int has_ext = dot && dot != base && dot[1] &&
                  (size_t)(base + strlen(base) - dot) <= 9;
    int sl = has_ext ? (int)(dot - base) : (int)strlen(base);
    if (sl > (int)sizeof(stem) - 1) sl = sizeof(stem) - 1;
    memcpy(stem, base, (size_t)sl); stem[sl] = 0;
    if (has_ext) {
        strncpy(ext, dot, sizeof(ext) - 1); ext[sizeof(ext) - 1] = 0;
    }
    /* sanitize stem: keep [A-Za-z0-9._-], fold the rest to '_' */
    for (char *c = stem; *c; c++) {
        char ch = *c;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            *c = '_';
    }
    if (!stem[0]) strcpy(stem, "file");
    /* downloads live in the user's own folder (named after the account),
     * falling back to "Downloads" when no user name is set */
    char dl_dir[24];
    const char *un = gui_user_name();
    if (un && un[0]) {
        strncpy(dl_dir, un, sizeof(dl_dir) - 1); dl_dir[sizeof(dl_dir)-1] = 0;
        for (char *c = dl_dir; *c; c++)
            if (!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||
                  (*c>='0'&&*c<='9')||*c=='-'||*c=='_')) *c='_';
    } else strcpy(dl_dir, "Downloads");
    strncpy(g_dl_dir, dl_dir, sizeof(g_dl_dir) - 1);
    g_dl_dir[sizeof(g_dl_dir)-1] = 0;
    if (vfs_find(dl_dir) < 0) vfs_create(dl_dir, VFS_DIR);
    /* stem+ext must fit VFS_NAME_LEN (24): trim stem to make room */
    int extl = (int)strlen(ext);
    int maxstem = 23 - extl;
    if ((int)strlen(stem) > maxstem) stem[maxstem] = 0;
    if (!stem[0]) strcpy(stem, "file");
    char name[32];
    int rc = -1;
    for (int i = 0; i < 100; i++) {
        if (i == 0) snprintf(name, sizeof(name), "%s%s", stem, ext);
        else snprintf(name, sizeof(name), "%.*s-%d%s",
                      maxstem - 3 > 0 ? maxstem - 3 : 1, stem, i, ext);
        name[sizeof(name) - 1] = 0;
        if (vfs_find_in(dl_dir, name) >= 0) continue;
        if (vfs_create_in(name, VFS_FILE, dl_dir) == 0) {
            rc = vfs_write_in(dl_dir, name, body, (int)blen);
            if (rc == 0 && outname && outcap > 0) {
                strncpy(outname, name, (size_t)outcap - 1);
                outname[outcap - 1] = 0;
            }
        }
        break;
    }
    return rc;
}

/* Built-in start page (voyager://start) - Voyager-style favorites dashboard,
 * rendered through the normal HTML engine so it looks/behaves native */
static const char START_PAGE_HTML[] =
"<html><head><style>"
"body{background-color:#F5F5F7;margin:0}"
".vt{color:#1D1D1F}.vs{color:#86868B}"
".tile{display:inline-block;width:148px;height:148px;border-radius:20px;"
"margin:8px;padding-top:52px;text-align:center;vertical-align:top}"
".tile a{color:#FFFFFF;font-size:17px;font-weight:bold;"
"text-decoration:none}"
".tile a:hover{color:#FFE9B0}"
"</style></head><body bgcolor=\"#F5F5F7\">"
"<div style='text-align:center;padding-top:56px'>"
"<div class='vt' style='font-size:64px;font-weight:bold;color:#0A84FF'>Voyager</div>"
"<div class='vs' style='font-size:17px;padding-top:6px'>"
"Your personal computer, reimagined.</div></div>"
"<div style='text-align:center;padding-top:26px;color:#86868B;font-size:13px'>"
"Type a web address in the bar above to surf the web</div>"
"<hr style='border:none;border-top:1px solid #D2D2D7;margin:26px 60px'>"
"<div style='text-align:center'>"
"<div style='font-size:12px;color:#86868B;letter-spacing:2px'>FAVORITES</div>"
"<div class='tile' style='background:#1D1D1F'><a href='http://www.wikipedia.org/'>Wikipedia</a></div>"
"<div class='tile' style='background:#0A84FF'><a href='http://github.com/'>GitHub</a></div>"
"<div class='tile' style='background:#34C759'><a href='http://news.ycombinator.com/'>Hacker News</a></div>"
"<div class='tile' style='background:#FF9F0A'><a href='http://archive.org/'>Internet Archive</a></div>"
"<div class='tile' style='background:#FF375F'><a href='http://www.openstreetmap.org/'>OpenStreetMap</a></div>"
"<div class='tile' style='background:#5E5CE6'><a href='http://www.rfc-editor.org/'>RFC Editor</a></div>"
"<div class='tile' style='background:#30B0C7'><a href='http://www.iana.org/'>IANA</a></div>"
"<div class='tile' style='background:#BF5AF2'><a href='http://www.w3.org/'>W3C</a></div>"
"<div class='tile' style='background:#64D2FF'><a href='http://neverssl.com/'>Neverssl</a></div>"
"<div class='tile' style='background:#98989D'><a href='http://example.com/'>Example.com</a></div>"
"</div>"
"<div style='text-align:center;padding-top:34px;font-size:12px;color:#AEAEB2'>"
"Voyager OS</div>"
"</body></html>";

static void voyageweb_fetch(Tab *t) {
    char host[256];
    const char *path;
    char cur[600];
    strncpy(cur, t->url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = 0;

    /* built-in pages need no network */
    if (!strncmp(cur, "voyager://", 9)) {
        int w = t->last_w > 0 ? t->last_w : 800;
        if (t->page) { html2_free(t->page); t->page = NULL; }
        img_cache_clear(t);
        t->page = html2_parse(START_PAGE_HTML, sizeof(START_PAGE_HTML) - 1, w);
        t->state = ST_DONE;
        snprintf(t->status, sizeof(t->status), "Voyager Start Page");
        if (t->page && t->zoom != 100) html2_set_zoom(t->page, t->zoom);
        return;
    }

    if (!net_ready()) {
        t->state = ST_ERROR;
        snprintf(t->status, sizeof(t->status), "No network");
        return;
    }
    t->state = ST_LOADING;
    t->scroll = 0;

    /* follow redirects (301/302/303/307/308) — up to 5 hops; the address
     * bar ends up showing the FINAL url, like a real browser */
    HttpResp r;
    memset(&r, 0, sizeof(r));
    int got = 0;
    for (int hop = 0; hop < 5; hop++) {
        url_split(cur, host, sizeof(host), &path);
        snprintf(t->status, sizeof(t->status), "Connecting to %s...", host);
        gui_set_cursor(CUR_BUSY);
        http_free(&r);
        memset(&r, 0, sizeof(r));
        int rc = http_get(host, 80, path, &r, 20000);
        gui_set_cursor(-1);
        if (rc != 0) {
            t->state = ST_ERROR;
            snprintf(t->status, sizeof(t->status), "Cannot connect (err %d)", rc);
            return;
        }
        if (r.status == 301 || r.status == 302 || r.status == 303 ||
            r.status == 307 || r.status == 308) {
            const char *loc = http_header(&r, "location");
            char nxt[640];
            if (loc && loc[0]) {
                url_resolve_base(cur, loc, nxt, sizeof(nxt));
                if (nxt[0] && strncasecmp(nxt, "http://", 7)) {
                    /* non-http redirect target: give up gracefully */
                } else if (nxt[0]) {
                    strncpy(cur, nxt, sizeof(cur) - 1); cur[sizeof(cur) - 1] = 0;
                    continue;
                }
            }
        }
        got = 1;
        break;
    }
    (void)got;
    /* reflect the final URL in the address bar / page URL */
    if (strcmp(cur, t->url)) {
        strncpy(t->url, cur, sizeof(t->url) - 1); t->url[sizeof(t->url) - 1] = 0;
        strncpy(t->url_edit, cur, sizeof(t->url_edit) - 1); t->url_edit[sizeof(t->url_edit) - 1] = 0;
        t->url_caret = (int)strlen(t->url_edit);
        t->url_sel_a = -1;
    }
    url_split(cur, host, sizeof(host), &path);
    if (r.status >= 400) {
        t->state = ST_ERROR;
        snprintf(t->status, sizeof(t->status), "HTTP %d", r.status);
        http_free(&r);
        return;
    }

    bool is_html = strstr(r.content_type, "html") ||
                   strstr(r.content_type, "text/") || r.body_len == 0;
    /* servers that send no/wrong Content-Type: sniff the body - a leading
     * '<' after whitespace almost always means markup, not a binary file */
    if (!is_html && r.body_len > 0 && r.body_len < (1u << 20)) {
        uint32_t i = 0;
        while (i < r.body_len &&
               (r.body[i] == ' ' || r.body[i] == '\t' || r.body[i] == '\r' ||
                r.body[i] == '\n')) i++;
        if (i < r.body_len && r.body[i] == '<') is_html = true;
    }
    /* textual non-html types still render as a text page; everything else
     * (octet-stream, pdf, zip, images served directly, ...) downloads */
    bool textual = strstr(r.content_type, "text/") ||
                   strstr(r.content_type, "html") ||
                   strstr(r.content_type, "json") ||
                   strstr(r.content_type, "xml") ||
                   strstr(r.content_type, "javascript") ||
                   strstr(r.content_type, "css") ||
                   strstr(r.content_type, "csv");
    /* fetch runs from voyageweb_update where g_cur_win is NULL, so
     * app_content_w() is 0 and every page used to parse at the 200px
     * floor. Reuse the width from the last draw instead. */
    int w = t->last_w > 0 ? t->last_w : 800;
    t->last_w = w;

    if (t->page) { html2_free(t->page); t->page = NULL; }
    img_cache_clear(t);
    t->mem_warn = 0;

    t->page_bytes = r.body_len;   /* html/text payload; images add on top */
    if (is_html && r.body_len > 0) {
        load_page_images(t, r.body, r.body_len);
        HtmlImgRef refs[IMG_MAX];
        int nr = 0;
        for (int i = 0; i < t->nimgs && nr < IMG_MAX; i++) {
            if (!t->imgs[i].rgba) continue;
            refs[nr].url = t->imgs[i].src;
            refs[nr].rgba = t->imgs[i].rgba;
            refs[nr].w = t->imgs[i].w;
            refs[nr].h = t->imgs[i].h;
            nr++;
        }
        t->page = html2_parse2(r.body, r.body_len, w, refs, nr);
        if (t->page) {
            size_t dom_b = html2_mem_bytes(t->page);
            if (dom_b > VOYAGEWEB_DOM_MEM_KILL) {
                /* page is too demanding: kill it outright and say so
                 * instead of letting it freeze the whole OS */
                html2_free(t->page); t->page = NULL;
                snprintf(t->status, sizeof(t->status),
                         "Memory exceeded - page too demanding");
                const char *ep =
                    "<html><body style='font-family:Helvetica;padding:30px'>"
                    "<h2>Memory exceeded</h2>"
                    "<p>This page needed more memory than Voyager OS "
                    "allows, so it was not displayed.</p></body></html>";
                t->page = html2_parse(ep, strlen(ep), w);
            } else if (dom_b > VOYAGEWEB_DOM_MEM_WARN) {
                t->mem_warn = 1;   /* DOM itself is huge - warn in chrome */
            }
        }
    } else if (!textual && r.body_len > 0) {
        /* binary/file response: save it to the user's folder instead of
         * dumping raw bytes as text. Disk-space guard first: a file needs
         * one contiguous free run on the VDI - refuse cleanly if it
         * won't fit rather than failing mid-write */
        int need_sec = (int)((r.body_len + 511) / 512);
        if (need_sec > vfs_free_run_sectors()) {
            snprintf(t->status, sizeof(t->status),
                     "Not enough disk space for this download");
            t->state = ST_ERROR;
            char full[256];
            snprintf(full, sizeof(full),
                     "<html><body style='font-family:Helvetica;padding:24px'>"
                     "<h2>Download failed</h2>"
                     "<p>There is not enough free space on this disk for "
                     "%u KB.</p></body></html>",
                     (unsigned)(r.body_len / 1024));
            t->page = html2_parse(full, strlen(full), w);
        } else {
        char fname[40];
        int drc = voyageweb_save_download(path, r.body, r.body_len,
                                       fname, sizeof(fname));
        if (drc == 0) {
            snprintf(t->status, sizeof(t->status),
                     "Downloaded %s (%u KB) to %s/", fname,
                     (unsigned)(r.body_len / 1024), g_dl_dir);
            char dl[256];
            snprintf(dl, sizeof(dl),
                     "<html><body style='font-family:Helvetica;padding:24px'>"
                     "<h2>Download complete</h2>"
                     "<p><b>%s</b> (%u bytes)</p>"
                     "<p>Saved to <b>%s/%s</b> - open it in Finder.</p>",
                     fname, (unsigned)r.body_len, g_dl_dir, fname);
            t->page = html2_parse(dl, strlen(dl), w);
        } else {
            snprintf(t->status, sizeof(t->status),
                     "Download failed - disk full or file limit reached");
            t->state = ST_ERROR;
        }
        }
    } else {
        size_t cap = r.body_len * 2 + 256;
        char *buf = (char *)kmalloc(cap);
        if (buf) {
            int n = snprintf(buf, cap, "<html><body style='font-family:monospace;white-space:pre'>");
            for (uint32_t i = 0; i < r.body_len && n < (int)cap - 8; i++) {
                char ch = r.body[i];
                if (ch == '<') { buf[n++]='&'; buf[n++]='l'; buf[n++]='t'; buf[n++]=';'; }
                else if (ch == '>') { buf[n++]='&'; buf[n++]='g'; buf[n++]='t'; buf[n++]=';'; }
                else if (ch == '&') { buf[n++]='&'; buf[n++]='a'; buf[n++]='m'; buf[n++]='p'; buf[n++]=';'; }
                else buf[n++] = ch;
            }
            snprintf(buf + n, cap - n, "</body></html>");
            t->page = html2_parse(buf, strlen(buf), w);
            kfree(buf);
        }
    }
    http_free(&r);
    /* apply the tab's zoom (parse lays out at 100%, relayout if needed) */
    if (t->page && t->zoom != 100) html2_set_zoom(t->page, t->zoom);
    t->state = t->page ? ST_DONE : ST_ERROR;
    if (!t->page)
        snprintf(t->status, sizeof(t->status), "Cannot render page");
    else {
        html2_js_set_url(t->page, t->url);
        /* status = "host - title"; the fetched byte count is drawn
         * right-aligned in the status bar (see voyageweb_draw) */
        char tt[160];
        snprintf(tt, sizeof(tt), "%s  %s", host,
                 t->page->title[0] ? t->page->title : "");
        strncpy(t->status, tt, sizeof(t->status)-1);
        t->status[sizeof(t->status)-1] = 0;
    }
}

/* ---------- geometry ---------- */
static int toolbar_y(void) { return TAB_H; }
static int cy0_now(void) { return TAB_H + TOOLBAR_H + PAD_T; }
static int view_h_now(void) {
    int H = app_content_h();
    return H - cy0_now() - STATUS_H - PAD_T;
}
static int pw_now(void) { int w = app_content_w() - 2 * PAD_L; return w < 200 ? 200 : w; }

static bool clip_rect(int *x, int *y, int *w, int *h,
                      int vx, int vy, int vw, int vh) {
    int x0 = *x < vx ? vx : *x;
    int y0 = *y < vy ? vy : *y;
    int x1 = *x + *w > vx + vw ? vx + vw : *x + *w;
    int y1 = *y + *h > vy + vh ? vy + vh : *y + *h;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* ---------- tab strip ---------- */
/* Layout metrics shared by draw + hit-testing. Returns 1 when the strip
 * overflows and < > scroll buttons are shown. */
static int tab_metrics(VoyageWebCtx *c, int *tw_out, int *vis_out, int *maxsc_out) {
    int W = app_content_w();
    int n = c->ntabs > 0 ? c->ntabs : 1;
    /* two-pass: first assume no scroll buttons; if the strip still
     * overflows, reserve room for < > and recompute - the old single
     * estimate could leave tabs clipped with no way to reach them */
    int reserve = 44;
    int tw, vis, maxsc;
    for (int pass = 0; pass < 2; pass++) {
        vis = W - 8 - reserve;
        if (vis < 80) vis = 80;
        tw = vis / n;
        if (tw > 190) tw = 190;
        if (tw < 44) tw = 44;
        int total = n * (tw + 3);
        maxsc = total > vis ? total - vis : 0;
        if (c->tab_scroll > maxsc) c->tab_scroll = maxsc;
        if (c->tab_scroll < 0) c->tab_scroll = 0;
        if (maxsc > 0 && reserve == 44) { reserve = 92; continue; }
        break;
    }
    *tw_out = tw; *vis_out = vis; *maxsc_out = maxsc;
    return maxsc > 0;
}

/* returns tab index, -2 = "+" new-tab, -3 = "<", -4 = ">", -1 = none */
static int tab_hit(VoyageWebCtx *c, int x, int y) {
    if (y < 0 || y >= TAB_H) return -1;
    int W = app_content_w();
    int tw, vis, maxsc;
    int scr = tab_metrics(c, &tw, &vis, &maxsc);
    if (scr) {
        if (x >= W - 86 && x < W - 64) return -3;
        if (x >= W - 62 && x < W - 40) return -4;
    }
    if (x >= W - 36) return -2;
    int tx = 4 - c->tab_scroll;
    for (int i = 0; i < c->ntabs; i++) {
        if (x >= tx && x < tx + tw && tx + tw > 4 && tx < 4 + vis)
            return i;
        tx += tw + 3;
    }
    return -1;
}
static int tab_close_hit(VoyageWebCtx *c, int x, int y, int i) {
    (void)y;
    int tw, vis, maxsc;
    tab_metrics(c, &tw, &vis, &maxsc);
    int tx = 4 + i * (tw + 3) - c->tab_scroll;
    if (tx + tw <= 4 || tx >= 4 + vis) return 0;
    return x >= tx + tw - 20 && x < tx + tw - 4;
}
static void tab_scroll_by(VoyageWebCtx *c, int dir) {
    int tw, vis, maxsc;
    tab_metrics(c, &tw, &vis, &maxsc);
    /* dock-style paging: one click scrolls almost a full screenful of
     * tabs (visible count - 1), landing on tab boundaries */
    int per = vis / (tw + 3);
    if (per < 1) per = 1;
    int step = (per > 1 ? per - 1 : 1) * (tw + 3);
    c->tab_scroll += dir * step;
    if (c->tab_scroll < 0) c->tab_scroll = 0;
    if (c->tab_scroll > maxsc) c->tab_scroll = maxsc;
}

/* keep the active tab inside the visible strip window */
static void tab_ensure_visible(VoyageWebCtx *c, int idx) {
    int tw, vis, maxsc;
    tab_metrics(c, &tw, &vis, &maxsc);
    if (maxsc <= 0) { c->tab_scroll = 0; return; }
    int left = 4 + idx * (tw + 3);
    int right = left + tw;
    if (left - c->tab_scroll < 4)
        c->tab_scroll = left - 4;
    else if (right - c->tab_scroll > 4 + vis)
        c->tab_scroll = right - (4 + vis);
    if (c->tab_scroll < 0) c->tab_scroll = 0;
    if (c->tab_scroll > maxsc) c->tab_scroll = maxsc;
}

static void tab_new(VoyageWebCtx *c) {
    if (c->ntabs >= c->nalloc) {
        int nc = c->nalloc * 2;
        Tab **nt = (Tab **)krealloc(c->tabs, sizeof(Tab *) * (size_t)nc);
        if (!nt) return;             /* out of memory: keep what we have */
        memset(nt + c->nalloc, 0, sizeof(Tab *) * (size_t)(nc - c->nalloc));
        c->tabs = nt; c->nalloc = nc;
    }
    Tab *t = tab_alloc();
    if (!t) {
        kprintf("voyageweb: tab alloc FAIL ntabs=%d used=%uKB\n",
                c->ntabs, (unsigned)(kmalloc_used() / 1024));
        return;
    }
    snprintf(t->status, sizeof(t->status), "New Tab");
    c->tabs[c->ntabs] = t;
    int ni = c->ntabs++;
    voyageweb_set_active(c, ni);
    tab_ensure_visible(c, c->active);
}

static void tab_close(VoyageWebCtx *c, int i) {
    if (i < 0 || i >= c->ntabs) return;
    Tab *dead = c->tabs[i];
    tab_clear(dead);
    kfree(dead);
    for (int j = i; j < c->ntabs - 1; j++) c->tabs[j] = c->tabs[j+1];
    c->ntabs--;
    if (c->ntabs <= 0) {
        c->ntabs = 1; c->active = 0;
        c->tabs[0] = tab_alloc();
        if (c->tabs[0]) snprintf(c->tabs[0]->status,
                                 sizeof(c->tabs[0]->status), "New Tab");
    }
    int na = c->active;
    if (na > i) na--;
    if (na >= c->ntabs) na = c->ntabs - 1;
    voyageweb_set_active(c, na);
    tab_ensure_visible(c, c->active);
}

static const char *tab_title(Tab *t, char *buf, int cap) {
    if (t->page && t->page->title[0]) {
        strncpy(buf, t->page->title, cap-1); buf[cap-1]=0; return buf;
    }
    if (t->url[0]) {
        char host[128]; const char *path;
        url_split(t->url, host, sizeof(host), &path);
        snprintf(buf, cap, "%s", host);
        return buf;
    }
    snprintf(buf, cap, "New Tab");
    return buf;
}

/* ---------- drawing ---------- */

static void draw_widget(VoyageWebCtx *c, HtmlPage *pg, HtmlPrim *p, int sx, int sy) {
    (void)c;
    HtmlNode *n = html2_node(pg, p->elem);
    if (!n) return;
    bool foc = (pg->focus_elem == p->elem);
    uint32_t border = foc ? rgb(138, 0, 255) : rgb(190, 190, 195);
    uint32_t face = rgb(255, 255, 255);
    uint32_t txt = rgb(30, 30, 35);
    int fh = p->scale; if (fh < 8) fh = 8;      /* widget glyph px height */
    int adv = fh + fh / 8;                       /* per-char advance */
    switch (p->wkind) {
    case WK_BUTTON: {
        const char *ty = n->type_a ? n->type_a : "submit";
        /* <input> labels live in value= (live); <button> labels live in
         * child text captured at layout (p->text). Using the wrong one
         * showed "Submit" for <button>Ribbit</button> or stale text */
        const char *lbl = NULL;
        if (n->tag && !strcmp(n->tag, "input") && n->value && n->value[0])
            lbl = n->value;
        else if (p->text && p->text[0]) lbl = p->text;
        else lbl = n->value;
        if (!lbl) {
            if (!strcasecmp(ty, "submit")) lbl = "Submit";
            else if (!strcasecmp(ty, "reset")) lbl = "Reset";
            else lbl = "Button";
        }
        /* press/hover feedback: held = darker inset face + label shifts
         * down-right like a real button; hovered = light blue tint */
        bool prs = p->wstate & 1, hov = p->wstate & 2;
        if (prs) {
            app_fill(sx, sy, p->w, p->h, rgb(180, 180, 188));
            app_rect(sx, sy, p->w, p->h, rgb(120, 120, 128));
            app_fill(sx + 1, sy + 1, p->w - 2, p->h - 2, rgb(215, 215, 222));
        } else {
            app_fill(sx, sy, p->w, p->h,
                     hov ? rgb(233, 215, 245) : rgb(230, 230, 235));
            app_rect(sx, sy, p->w, p->h, border);
            app_fill(sx + 1, sy + 1, p->w - 2, p->h - 2,
                     hov ? rgb(240, 248, 255) : face);
        }
        int tw = (int)strlen(lbl) * adv;
        int lx = sx + (p->w - tw) / 2 + (prs ? 1 : 0);
        int ly = sy + (p->h - fh) / 2 + (prs ? 1 : 0);
        app_text_q(lx, ly, lbl, txt, fh);
        break;
    }
    case WK_TEXT: case WK_TEXTAREA: case WK_SELECT: {
        app_fill(sx, sy, p->w, p->h, face);
        app_rect(sx, sy, p->w, p->h, border);
        const char *v = n->value ? n->value : (n->placeholder ? n->placeholder : "");
        uint32_t col = v && v[0] ? txt : rgb(150, 150, 155);
        /* selection highlight behind the text */
        if (n->sel_a >= 0 && n->caret >= 0 && n->sel_a != n->caret && v) {
            int len = (int)strlen(v);
            int a = n->sel_a < n->caret ? n->sel_a : n->caret;
            int b = n->sel_a < n->caret ? n->caret : n->sel_a;
            if (a < 0) a = 0; if (b > len) b = len;
            if (b > a) {
                int x0 = sx + 5 + a * adv;
                int x1 = sx + 5 + b * adv;
                if (x1 > sx + p->w - 4) x1 = sx + p->w - 4;
                app_fill(x0, sy + 3, x1 - x0, p->h - 6, rgb(226, 190, 250));
            }
        }
        if (v && v[0])
            app_text_clip_q(sx + 5, sy + (p->h - fh) / 2, v, col, fh,
                            sx, sy, p->w - 8, p->h - 4);
        if (p->wkind == WK_SELECT) {
            int ax = sx + p->w - adv - 4;
            app_text_q(ax, sy + (p->h - fh) / 2, "v", txt, fh);
        }
        if (foc && p->wkind != WK_SELECT && (gui_ticks() / 500) % 2 == 0) {
            int cx = sx + 5;
            if (v && n->caret >= 0) {
                char tmp[128];
                int cl = n->caret; if (cl > 120) cl = 120;
                memcpy(tmp, v, cl); tmp[cl] = 0;
                cx = sx + 5 + (int)strlen(tmp) * adv;
            }
            if (cx < sx + p->w - 4)
                app_fill(cx, sy + 4, 1, p->h - 8, rgb(30, 30, 35));
        }
        break;
    }
    case WK_CHECKBOX: {
        int s = p->h < p->w ? p->h : p->w;
        if (s > 16) s = 16;
        int bx = sx, by = sy + (p->h - s) / 2;
        app_fill(bx, by, s, s,
                 (p->wstate & 1) ? rgb(226, 190, 250) : face);
        app_rect(bx, by, s, s, rgb(120, 120, 125));
        if (n->checked) {
            app_fill(bx + 3, by + 3, s - 6, 2, rgb(138, 0, 255));
            app_fill(bx + 3, by + s - 5, s - 6, 2, rgb(138, 0, 255));
            app_fill(bx + s - 5, by + 3, 2, s - 6, rgb(138, 0, 255));
        }
        if (n->value && n->value[0])
            app_text_clip_q(sx + s + 5, sy + (p->h - fh) / 2, n->value, txt, fh,
                            sx, sy, p->w, p->h);
        break;
    }
    case WK_RADIO: {
        int s = p->h < p->w ? p->h : p->w;
        if (s > 16) s = 16;
        int bx = sx, by = sy + (p->h - s) / 2;
        app_fill(bx, by, s, s,
                 (p->wstate & 1) ? rgb(226, 190, 250) : face);
        app_rect(bx, by, s, s, rgb(120, 120, 125));
        if (n->checked)
            app_fill(bx + 4, by + 4, s - 8, s - 8, rgb(138, 0, 255));
        if (n->value && n->value[0])
            app_text_clip_q(sx + s + 5, sy + (p->h - fh) / 2, n->value, txt, fh,
                            sx, sy, p->w, p->h);
        break;
    }
    case WK_MARQUEE: {
        const char *v = n->value ? n->value : "";
        if (v[0]) app_text_clip_q(sx, sy + (p->h - fh) / 2, v, txt, fh,
                                  sx, sy, p->w, p->h);
        break;
    }
    default: break;
    }
}

/* scaled RGBA->XRGB blit straight into the framebuffer.
 * (gx,gy,dw,dh) is the VISIBLE dest rect (already clipped to the
 * viewport); (fx0,fy0,fw,fh) is the FULL unclipped dest rect so the
 * source sample position accounts for whatever clip_rect cut away -
 * previously a half-scrolled image was squished into the visible
 * strip instead of cropped, which is what made edges look broken. */
static void blit_img(int gx, int gy, int dw, int dh,
                     const uint8_t *rgba, int sw, int sh,
                     int fx0, int fy0, int fw, int fh) {
    if (!rgba || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        fw <= 0 || fh <= 0) return;
    uint32_t *fb; uint32_t pitch;
    fb_get_target(&fb, &pitch);
    if (!fb || pitch < 8) return;
    /* clamp the visible rect to the actual draw surface - during the
     * spin-out snapshot the target is a small window-sized buffer and
     * unclamped writes corrupted neighbouring heap */
    int surfw = (int)fb_width(), surfh = (int)fb_height();
    if (gx < 0) { dw += gx; gx = 0; }
    if (gy < 0) { dh += gy; gy = 0; }
    if (gx + dw > surfw) dw = surfw - gx;
    if (gy + dh > surfh) dh = surfh - gy;
    if (dw <= 0 || dh <= 0) return;
    int xoff = gx - fx0, yoff = gy - fy0;   /* dest px inside full rect */
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
            uint32_t d = *row;
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t nr = (px[0] * a + dr * (255 - a)) / 255;
            uint32_t ng = (px[1] * a + dg * (255 - a)) / 255;
            uint32_t nb = (px[2] * a + db * (255 - a)) / 255;
            *row++ = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* pick the current frame of an animated GIF by wall clock. Delays are
 * GCE hundredths-of-a-second, clamped to a sane minimum like browsers. */
static int anim_frame_at(VoyageWebImg *im, uint64_t now) {
    if (!im->frames || im->nframes <= 1) return 0;
    int total = 0;
    for (int i = 0; i < im->nframes; i++) {
        int ms = im->frames[i].delay_cs * 10;
        if (ms < 20) ms = 20;
        total += ms;
    }
    if (total <= 0) return 0;
    int64_t e = (int64_t)(now - im->anim_t0) % total;
    if (e < 0) e = 0;
    for (int i = 0; i < im->nframes; i++) {
        int ms = im->frames[i].delay_cs * 10;
        if (ms < 20) ms = 20;
        e -= ms;
        if (e < 0) return i;
    }
    return im->nframes - 1;
}

/* ---------- url field editing ---------- */
#define CHW 9   /* approx char advance at scale 1 */

static int url_text_x(Tab *t, int ux, int uw) {
    int tw = app_text_width(t->url_edit, 1);
    int tx = ux + 10;
    if (tw > uw - 20) {
        /* overflow: scroll horizontally so the CARET stays visible -
         * text cuts off at the field edge and follows the cursor
         * instead of pinning to the tail */
        int caret_px = t->url_caret * CHW;
        int right = ux + uw - 14, left = ux + 12;
        if (tx + caret_px > right) tx = right - caret_px;
        if (tx + caret_px < left)  tx = left - caret_px;
        if (tx > ux + 10) tx = ux + 10;   /* never pad empty space left */
    }
    return tx;
}
static int url_char_at(Tab *t, int x, int tx) {
    int c = (x - tx) / CHW;
    int len = (int)strlen(t->url_edit);
    if (c < 0) c = 0;
    if (c > len) c = len;
    return c;
}
static void url_sel_del(Tab *t) {
    if (t->url_sel_a < 0 || t->url_sel_a == t->url_caret) { t->url_sel_a = -1; return; }
    char *s = t->url_edit;
    int a = t->url_sel_a < t->url_caret ? t->url_sel_a : t->url_caret;
    int b = t->url_sel_a < t->url_caret ? t->url_caret : t->url_sel_a;
    int len = (int)strlen(s);
    if (b > len) b = len;
    memmove(s + a, s + b, len - b + 1);
    t->url_caret = a;
    t->url_sel_a = -1;
}
static void url_insert_ch(Tab *t, char ch) {
    if (ch < 32 || ch > 126) return;
    url_sel_del(t);
    char *s = t->url_edit;
    int len = (int)strlen(s);
    if (len >= 500) return;
    int at = t->url_caret; if (at < 0) at = len; if (at > len) at = len;
    memmove(s + at + 1, s + at, len - at + 1);
    s[at] = ch;
    t->url_caret = at + 1;
}
static void url_backspace(Tab *t) {
    if (t->url_sel_a >= 0 && t->url_sel_a != t->url_caret) { url_sel_del(t); return; }
    int at = t->url_caret;
    char *s = t->url_edit;
    if (at <= 0) { t->url_sel_a = -1; return; }
    int len = (int)strlen(s);
    memmove(s + at - 1, s + at, len - at + 1);
    t->url_caret = at - 1;
    t->url_sel_a = -1;
}
static void url_delete(Tab *t) {
    if (t->url_sel_a >= 0 && t->url_sel_a != t->url_caret) { url_sel_del(t); return; }
    char *s = t->url_edit;
    int at = t->url_caret;
    int len = (int)strlen(s);
    if (at < 0) at = len;
    if (at >= len) { t->url_sel_a = -1; return; }
    memmove(s + at, s + at + 1, len - at);
    t->url_sel_a = -1;
}
static void url_key(Tab *t, int k) {
    int len = (int)strlen(t->url_edit);
    int at = t->url_caret; if (at < 0) at = len; if (at > len) at = len;
    t->url_sel_a = -1;
    switch (k) {
    case 37: if (at > 0) t->url_caret = at - 1; break;
    case 39: if (at < len) t->url_caret = at + 1; break;
    case 36: t->url_caret = 0; break;
    case 35: t->url_caret = len; break;
    default: break;
    }
}

/* ---------- <select> dropdown ---------- */
#define SEL_ROW_H 18
#define SEL_MAX_SHOW 14

static int sel_dropdown_rows(VoyageWebCtx *c) {
    if (c->sel_open < 0 || !tab(c)->page) return 0;
    int opts[SEL_MAX_SHOW];
    return html2_select_options(tab(c)->page, c->sel_open, opts, SEL_MAX_SHOW);
}

/* which option row is under screen point (ex,ey), or -1 */
static int sel_row_at(VoyageWebCtx *c, int ex, int ey) {
    int n = sel_dropdown_rows(c);
    if (n <= 0) return -1;
    int dy = c->sel_y + c->sel_h;
    int ch = app_content_y() + app_content_h();
    int dh = n * SEL_ROW_H + 4;
    if (dy + dh > ch) dy = c->sel_y - dh;   /* flip above the widget */
    if (ex < c->sel_x || ex >= c->sel_x + c->sel_w) return -1;
    if (ey < dy || ey >= dy + dh) return -1;
    int r = (ey - dy - 2) / SEL_ROW_H;
    return (r >= 0 && r < n) ? r : -1;
}

static void sel_close(VoyageWebCtx *c) { c->sel_open = -1; c->sel_hover = -1; }

static void draw_select_dropdown(VoyageWebCtx *c, Tab *t) {
    if (c->sel_open < 0 || !t->page || t->state != ST_DONE) return;
    HtmlPage *pg = t->page;
    int opts[SEL_MAX_SHOW];
    int n = html2_select_options(pg, c->sel_open, opts, SEL_MAX_SHOW);
    if (n <= 0) return;
    int dy = c->sel_y + c->sel_h;
    int ch = app_content_y() + app_content_h();
    int dh = n * SEL_ROW_H + 4;
    if (dy + dh > ch) dy = c->sel_y - dh;
    if (dy < app_content_y()) dy = app_content_y();
    int dx = c->sel_x;
    /* shadow-ish border + white body */
    app_fill(dx + 2, dy + 2, c->sel_w, dh, rgb(0, 0, 0) | 0x33000000);
    app_fill(dx, dy, c->sel_w, dh, rgb(255, 255, 255));
    app_rect(dx, dy, c->sel_w, dh, rgb(180, 180, 185));
    for (int i = 0; i < n; i++) {
        int ry = dy + 2 + i * SEL_ROW_H;
        HtmlNode *op = html2_node(pg, opts[i]);
        if (i == c->sel_hover || (op && op->selected))
            app_fill(dx + 1, ry, c->sel_w - 2, SEL_ROW_H - 1, rgb(246, 230, 255));
        char txt[160];
        html2_node_innertext(pg, opts[i], txt, sizeof txt);
        /* trim whitespace */
        char *s = txt; while (*s == ' ') s++;
        app_text(dx + 8, ry + 4, s, rgb(30, 30, 35), 1);
        if (op && op->selected) {   /* drawn checkmark (font is ASCII-only) */
            uint32_t bl = rgb(138, 0, 255);
            app_fill(dx + c->sel_w - 15, ry + 8, 4, 2, bl);
            app_fill(dx + c->sel_w - 13, ry + 10, 4, 2, bl);
            app_fill(dx + c->sel_w - 11, ry + 12, 4, 2, bl);
            app_fill(dx + c->sel_w - 9,  ry + 14, 4, 2, bl);
        }
    }
}

static void voyageweb_draw(void *ctx) {
    VoyageWebCtx *c = (VoyageWebCtx *)ctx;
    Tab *t = tab(c);
    int W = app_content_w(), H = app_content_h();
    int pw = pw_now();

    if (t->page && t->last_w != pw) {
        /* keep the same relative position through the reflow so
         * resizing doesn't make the page jump to a different spot */
        int old_h = t->page->page_h, os = t->scroll;
        t->last_w = pw;
        html2_relayout(t->page, pw);
        int nh = t->page->page_h;
        if (old_h > 0 && nh > 0 && nh != old_h)
            t->scroll = (int)((int64_t)os * nh / old_h);
    }

    app_fill(0, 0, W, H, rgb(255, 255, 255));

    /* ---- tab strip (Voyager dark theme) ---- */
    app_fill(0, 0, W, TAB_H, rgb(53, 20, 58));
    app_fill(0, TAB_H - 1, W, 1, rgb(150, 70, 170));
    int tw, tvis, tmaxsc;
    int tscr = tab_metrics(c, &tw, &tvis, &tmaxsc);
    int tx = 4 - c->tab_scroll;
    for (int i = 0; i < c->ntabs; i++) {
        /* clip the tab's drawn rect to the visible strip window so a
         * partially-scrolled tab never paints over the < > / + controls */
        int dx0 = tx < 4 ? 4 : tx;
        int dx1 = tx + tw > 4 + tvis ? 4 + tvis : tx + tw;
        if (dx1 > dx0) {
            int dw = dx1 - dx0;
            bool act = (i == c->active);
            uint32_t bg = act ? rgb(73, 30, 80) : rgb(61, 24, 66);
            if (c->hov_tab == i && !act) bg = rgb(85, 34, 92);
            app_fill(dx0, 3, dw, TAB_H - 4, bg);
            app_rect(dx0, 3, dw, TAB_H - 4,
                     act ? rgb(172, 90, 190) : rgb(109, 52, 120));
            /* "#N title" - the tab number identifies it like a real
             * tabbed browser when dozens are open */
            char raw[80], tb[112];
            const char *title = tab_title(c->tabs[i], raw, sizeof(raw));
            snprintf(tb, sizeof(tb), "#%d %s", i + 1, title);
            tb[sizeof(tb) - 1] = 0;
            uint32_t tc = act ? rgb(250, 235, 255) : rgb(191, 140, 200);
            app_text_clip(tx + 10, 9, tb, tc, 1,
                          dx0 + 2, 4, dw - 30, TAB_H - 8);
            /* close x - only when its zone is inside the clip */
            int cx = tx + tw - 18;
            if (cx >= dx0 && cx + 14 <= dx1) {
                bool hovx = (c->hov_tab == i);
                if (hovx) app_fill(cx - 3, 8, 14, 14, rgb(150, 70, 170));
                app_text(cx, 9, "x", rgb(228, 190, 235), 1);
            }
        }
        tx += tw + 3;
    }
    /* active-tab indicator GLIDES to the newly selected tab (~200ms
     * ease-out) - a bright Voyager underline that travels the strip
     * instead of the highlight snapping between tabs */
    if (c->act_anim_t) {
        uint64_t el = gui_ticks() - c->act_anim_t;
        if (el >= 200) c->act_anim_t = 0;
        else {
            int x1 = tab_x_at(c, c->active);
            int e = 100 - (int)el;
            int ee = 100 - (e * e) / 100;      /* ease-out */
            int ex = c->act_x0 + (x1 - c->act_x0) * ee / 100;
            int iw = tw - 24; if (iw < 12) iw = 12;
            if (ex + iw > 4 && ex < 4 + tvis)
                app_fill(ex + 12, TAB_H - 4, iw, 3, rgb(216, 150, 240));
        }
    }
    /* tab scroll arrows (only when overflowing) */
    if (tscr) {
        int lx = W - 86, rx = W - 62;
        bool ldis = c->tab_scroll <= 0, rdis = c->tab_scroll >= tmaxsc;
        for (int b = 0; b < 2; b++) {
            int bx = b ? rx : lx;
            bool dis = b ? rdis : ldis;
            bool hov = !dis && c->hov_tab == (b ? -4 : -3);
            /* disabled = nearly-black face + dim glyph + faint border so
             * "no more tabs this way" is unmistakable at either end */
            uint32_t face = dis ? rgb(39, 16, 42) : hov ? rgb(128, 60, 140) : rgb(89, 40, 96);
            uint32_t edge = dis ? rgb(81, 38, 88) : rgb(172, 90, 190);
            uint32_t gl_c = dis ? rgb(103, 56, 110) : rgb(236, 200, 245);
            app_rounded(bx, 4, 20, TAB_H - 8, 6, face);
            app_rect(bx, 4, 20, TAB_H - 8, edge);
            const char *gl = b ? ">" : "<";
            int gw = app_text_width(gl, 1);
            app_text(bx + (20 - gw) / 2, 10, gl, gl_c, 1);
        }
    }
    /* "+" new tab */
    int nx = W - 32;
    if (c->hov_tab == -2) app_fill(nx - 2, 4, 30, TAB_H - 6, rgb(99, 46, 108));
    app_text(nx + 8, 9, "+", rgb(211, 160, 220), 2);

    /* ---- toolbar (Voyager dark) ---- */
    int ty = toolbar_y();
    app_fill(0, ty, W, TOOLBAR_H, rgb(39, 13, 42));
    app_rect(0, ty + TOOLBAR_H - 1, W, 1, rgb(150, 70, 170));

    bool can_back = t->hist_pos > 0;
    bool can_fwd = t->hist_pos < t->hist_n - 1;
    /* nav: "<" back, ">" forward (page history), "RELOAD" text — same
     * metrics as voyageweb_ui_hit/ux0_now */
    int bx1 = 10, bx2 = bx1 + NAV_W + 6, bx3 = bx2 + NAV_W + 6;
    int ux = bx3 + REL_W + 6;
    int uy = ty + 7, uh = 30;
    struct { int x, w, id; const char *lbl; } nb[3] = {
        { bx1, NAV_W, 1, "<" }, { bx2, NAV_W, 2, ">" }, { bx3, REL_W, 3, "RELOAD" }
    };
    for (int b = 0; b < 3; b++) {
        bool en = b == 0 ? can_back : b == 1 ? can_fwd : true;
        bool prs = c->prs_ui == nb[b].id;
        /* no hover face on the toolbar - plain like desktop icons;
         * press still sinks and the hand cursor marks it clickable */
        uint32_t face = !en ? rgb(65, 30, 70) : prs ? rgb(77, 34, 84) : rgb(99, 46, 108);
        app_rounded(nb[b].x, uy, nb[b].w, uh, 7, face);
        app_rect(nb[b].x, uy, nb[b].w, uh, rgb(202, 110, 220));
        uint32_t lc = !en ? rgb(141, 90, 150) : rgb(250, 235, 255);
        int lw = app_text_width(nb[b].lbl, 1);
        app_text(nb[b].x + (nb[b].w - lw) / 2, uy + 9, nb[b].lbl, lc, 1);
    }

    int uw = W - ux - 10;
    app_fill(ux, ty + 7, uw, 30, rgb(59, 24, 64));
    app_rect(ux, ty + 7, uw, 30,
             t->url_focused ? rgb(140, 245, 225) : rgb(172, 90, 190));
    /* url text + selection + caret */
    int tx0 = url_text_x(t, ux, uw);
    if (t->url_sel_a >= 0 && t->url_sel_a != t->url_caret) {
        int len = (int)strlen(t->url_edit);
        int a = t->url_sel_a < t->url_caret ? t->url_sel_a : t->url_caret;
        int b = t->url_sel_a < t->url_caret ? t->url_caret : t->url_sel_a;
        if (a < 0) a = 0; if (b > len) b = len;
        if (b > a) {
            int sx0 = tx0 + a * CHW, sx1 = tx0 + b * CHW;
            if (sx1 > ux + uw - 8) sx1 = ux + uw - 8;
            app_fill(sx0, ty + 9, sx1 - sx0, 26, rgb(150, 70, 170));
        }
    }
    app_text_clip(tx0, ty + 15, t->url_edit, rgb(243, 210, 250), 1,
                  ux, ty + 7, uw, 30);
    if (t->url_focused && (gui_ticks() / 500) % 2 == 0) {
        int cx = tx0 + t->url_caret * CHW;
        if (cx < ux + 4) cx = ux + 4;
        if (cx > ux + uw - 6) cx = ux + uw - 6;
        app_fill(cx, ty + 11, 1, 22, rgb(140, 245, 225));
    }

    /* url-field hover tint */
    if (c->hov_ui == 4) app_fill(ux, ty + 7, uw, 30, rgba(140, 245, 225, 24));

    /* ---- page content ---- */
    int cy0 = cy0_now();
    int vh = view_h_now();
    int page_h = t->page ? t->page->page_h : 0;
    int max_scroll = page_h - vh;
    if (max_scroll < 0) max_scroll = 0;
    if (t->scroll > max_scroll) t->scroll = max_scroll;
    if (t->scroll < 0) t->scroll = 0;

    if (t->state == ST_LOADING) {
        app_text(W / 2 - 40, cy0 + vh / 2, "Loading...", rgb(120, 120, 125), 1);
    } else if (t->state == ST_ERROR) {
        app_text(PAD_L, cy0, "!", rgb(255, 59, 48), 3);
        app_text(PAD_L + 30, cy0 + 10, t->status, rgb(200, 60, 50), 1);
        app_text(PAD_L, cy0 + 30, "Check the address and try again.", rgb(100, 100, 105), 1);
    } else if (t->page && t->page->nprim > 0) {
        HtmlPage *pg = t->page;
        int vx = PAD_L, vy = cy0, vw = pw, vh2 = vh;
        /* hover highlights the WHOLE link, not just the word under the
         * cursor — a link is one phrase ("Frog Find"), like a browser */
        int hov_link = -1;
        if (c->hov_prim >= 0 && c->hov_prim < pg->nprim)
            hov_link = pg->prims[c->hov_prim].link;
        for (int i = 0; i < pg->nprim; i++) {
            HtmlPrim *p = &pg->prims[i];
            int sy = p->y - t->scroll + cy0;
            if (sy + p->h < vy || sy > vy + vh2) continue;
            int sx = PAD_L + p->x;
            switch (p->type) {
            case RP_BG: case RP_RECT: case RP_RRECT: {
                int x = sx, y = sy, w = p->w, h = p->h;
                if (!clip_rect(&x, &y, &w, &h, vx, vy, vw, vh2)) break;
                uint32_t col = p->color;
                if (col >> 24 == 0) col |= 0xFF000000u;
                app_fill(x, y, w, h, col);
                break;
            }
            case RP_HLINE: {
                int x = sx, y = sy, w = p->w, h = p->h > 0 ? p->h : 1;
                if (!clip_rect(&x, &y, &w, &h, vx, vy, vw, vh2)) break;
                app_fill(x, y, w, h, p->color ? p->color : rgb(0,0,0));
                break;
            }
            case RP_TEXT: {
                if (!p->text || !p->text[0]) break;
                int fh = p->scale; if (fh < 4) fh = 4;   /* glyph px height */
                /* inline element background (<mark>, <span style=bg>) -
                 * color2 carries cs->bg from layout */
                uint32_t bg2 = p->color2;
                if (bg2 && (bg2 >> 24) != 0) {
                    int bx = sx, by = sy, bw = p->w, bh = p->h;
                    if (clip_rect(&bx, &by, &bw, &bh, vx, vy, vw, vh2))
                        app_fill(bx, by, bw, bh, bg2);
                }
                uint32_t col = p->color;
                if ((p->link >= 0 && p->link == hov_link && !(p->flags & 16)) ||
                    (p->link < 0 && i == c->hov_prim))
                    col = rgb(255, 0, 0) | 0xFF000000u;
                app_text_clip_q(sx, sy, p->text, col, fh, vx, vy, vw, vh2);
                /* real underline/strikethrough - flags were stored at
                 * layout but never drawn, so links didn't look like links */
                if (p->flags & 2) {   /* underline (text-decoration) */
                    int uy = sy + fh;
                    if (uy >= vy && uy < vy + vh2)
                        app_fill(sx, uy, p->w, 1, col);
                }
                if (p->flags & 8) {   /* line-through */
                    int uy = sy + fh / 2;
                    if (uy >= vy && uy < vy + vh2)
                        app_fill(sx, uy, p->w, 1, col);
                }
                break;
            }
            case RP_WIDGET: {
                HtmlPrim tp = *p;
                int x = sx, y = sy, w = p->w, h = p->h;
                if (!clip_rect(&x, &y, &w, &h, vx, vy, vw, vh2)) break;
                tp.x = x - PAD_L; tp.y = y - cy0 + t->scroll;
                tp.w = w; tp.h = h;
                /* widget feedback: bit0 = pressed (mouse held on it),
                 * bit1 = hovered - draw_widget renders the states */
                tp.wstate = 0;
                if (i == c->prs_prim) tp.wstate |= 1;
                else if (i == c->hov_prim) tp.wstate |= 2;
                draw_widget(c, pg, &tp, x, y);
                break;
            }
            case RP_IMG: {
                int fx = sx, fy = sy, fw = p->w, fh = p->h;
                int x = sx, y = sy, w = p->w, h = p->h;
                if (!clip_rect(&x, &y, &w, &h, vx, vy, vw, vh2)) break;
                /* animated GIF: p->img aliases frames[0]; find the owning
                 * cache entry and swap in the frame for this instant */
                const uint8_t *pix = p->img;
                for (int ii = 0; ii < t->nimgs; ii++) {
                    VoyageWebImg *im = &t->imgs[ii];
                    if (im->frames && im->rgba == p->img) {
                        int fi = anim_frame_at(im, gui_ticks());
                        if (im->frames[fi].rgba)
                            pix = im->frames[fi].rgba;
                        /* draw-time alpha check: proves what the blitter
                         * actually receives for the CURRENT frame */
                        if (fi != im->dbg_frame && pix && p->imgw > 0) {
                            im->dbg_frame = fi;
                            long z = 0, op = 0, tot = (long)p->imgw * p->imgh;
                            if (tot > 65536) tot = 65536;
                            for (long q = 0; q < tot; q++) {
                                uint32_t a = pix[q * 4 + 3];
                                if (a == 0) z++; else if (a == 255) op++;
                            }
                            kprintf("anim f%d %dx%d: zero=%d opaque=%d\n",
                                    fi, p->imgw, p->imgh, (int)z, (int)op);
                        }
                        break;
                    }
                }
                blit_img(app_content_x() + x, app_content_y() + y, w, h,
                         pix, p->imgw, p->imgh,
                         app_content_x() + fx, app_content_y() + fy, fw, fh);
                break;
            }
            default: break;
            }
        }
        if (pg->focus_elem >= 0) {
            for (int i = 0; i < pg->nprim; i++) {
                HtmlPrim *p = &pg->prims[i];
                if (p->type == RP_WIDGET && p->elem == pg->focus_elem) {
                    int sx = PAD_L + p->x, sy = p->y - t->scroll + cy0;
                    if (sy + p->h >= vy && sy <= vy + vh2) {
                        app_rect(sx - 1, sy - 1, p->w + 2, p->h + 2, rgb(138, 0, 255));
                        break;
                    }
                }
            }
        }
    } else {
        app_text(PAD_L, cy0, "Enter a URL above and press Enter.", rgb(120, 120, 125), 1);
        app_text(PAD_L, cy0 + 20, "Try: voyager://start or wikipedia.org", rgb(150, 150, 155), 1);
    }

    /* <select> dropdown paints above everything in the content area */
    draw_select_dropdown(c, t);

    /* busy cursor while a page loads; I-beam while the URL field has
     * focus; restore auto when neither applies */
    {
        static int cur_mode;   /* 0 auto 1 busy 2 ibeam */
        int want = t->state == ST_LOADING ? 1 : (t->url_focused ? 2 : 0);
        if (want != cur_mode) {
            cur_mode = want;
            gui_set_cursor(want == 1 ? CUR_BUSY : want == 2 ? CUR_IBEAM : -1);
        }
    }

    /* ---- warning strips over the top of the content ----
     * heavy page: DOM/images blew the per-tab budget (images were
     * dropped); unsynced clock: SNTP hasn't succeeded ~90s after boot
     * (no network or bad RTC) so timestamps/HTTPS can't be trusted */
    {
        int wy = cy0;
        if (t->mem_warn) {
            app_fill(PAD_L, wy, pw, 20, rgba(110, 75, 0, 225));
            app_rect(PAD_L, wy, pw, 20, rgb(190, 140, 20));
            app_text(PAD_L + 8, wy + 6,
                     "This page is using a lot of memory - some content may be missing",
                     rgb(255, 240, 200), 1);
            wy += 20;
        }
        if (!gui_time_synced() && gui_ticks() > 90000) {
            app_fill(PAD_L, wy, pw, 20, rgba(137, 50, 140, 225));
            app_rect(PAD_L, wy, pw, 20, rgb(192, 90, 200));
            app_text(PAD_L + 8, wy + 6,
                     "Clock not synchronized - check Date & Time in Settings",
                     rgb(251, 225, 255), 1);
        }
    }

    /* tab-switch page fade: the new page fades in over ~180ms in sync
     * with the strip indicator glide - the whole tab change animates,
     * not just the tab chrome */
    if (c->act_anim_t) {
        uint64_t el = gui_ticks() - c->act_anim_t;
        if (el < 180) {
            int a = (int)(170 * (180 - el) / 180);
            app_blend(0, cy0, W, vh, rgba(16, 6, 20, a));
        }
    }

    /* ---- status bar (Voyager dark) ----
     * left: host - title / message; right: fetched byte count so the
     * two pieces are evenly spaced across the bar */
    app_fill(0, H - STATUS_H, W, STATUS_H, rgb(39, 13, 42));
    app_fill(0, H - STATUS_H, W, 1, rgb(150, 70, 170));
    app_text(8, H - STATUS_H + 5, t->status, rgb(191, 140, 200), 1);
    if (t->state == ST_DONE && t->page_bytes > 0) {
        char kb[32];
        uint32_t bytes = (uint32_t)t->page_bytes;
        if (bytes >= 1048576)
            snprintf(kb, sizeof(kb), "[%u.%u MB]",
                     (unsigned)(bytes / 1048576),
                     (unsigned)((bytes % 1048576) * 10 / 1048576));
        else if (bytes >= 1024)
            snprintf(kb, sizeof(kb), "[%u.%u KB]",
                     (unsigned)(bytes / 1024),
                     (unsigned)((bytes % 1024) * 10 / 1024));
        else
            snprintf(kb, sizeof(kb), "[%u B]", (unsigned)bytes);
        int kw = app_text_width(kb, 1);
        app_text(W - 8 - kw, H - STATUS_H + 5, kb, rgb(191, 140, 200), 1);
    }
    if (max_scroll > 0) {
        int sy0 = cy0 + 4;
        int th = vh - 8;
        int thumb_h = th * vh / (page_h + 1);
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = sy0 + (th - thumb_h) * t->scroll / max_scroll;
        app_fill(W - 10, sy0, 5, th, rgb(73, 30, 80));
        app_fill(W - 10, thumb_y, 5, thumb_h, rgb(172, 90, 190));
    }
}

/* ---------- events ---------- */

static int voyageweb_ui_hit(VoyageWebCtx *c, int x, int y) {
    Tab *t = tab(c);
    (void)t;
    int ty = toolbar_y();
    if (y < ty + 7 || y >= ty + 37) return 0;
    int bx1 = 10, bx2 = bx1 + NAV_W + 6, bx3 = bx2 + NAV_W + 6;
    int ux = bx3 + REL_W + 6;
    if (x >= bx1 && x < bx1 + NAV_W) return 1;
    if (x >= bx2 && x < bx2 + NAV_W) return 2;
    if (x >= bx3 && x < bx3 + REL_W) return 3;
    if (x >= ux) return 4;
    return 0;
}

static int ux0_now(VoyageWebCtx *c) {
    (void)c;
    int bx1 = 10, bx2 = bx1 + NAV_W + 6, bx3 = bx2 + NAV_W + 6;
    return bx3 + REL_W + 6;
}

/* hit-test page coords; returns prim index or -1 */
static int page_hit(VoyageWebCtx *c, Tab *t, int ex, int ey, int *px, int *py) {
    if (!t->page || t->state != ST_DONE) return -1;
    int cy0 = cy0_now();
    if (ey < cy0 || ey >= cy0 + view_h_now()) return -1;
    if (ex < PAD_L || ex >= PAD_L + pw_now()) return -1;
    *px = ex - PAD_L;
    *py = ey - cy0 + t->scroll;
    return html2_hit(t->page, *px, *py);
}

/* nearest ancestor <a href> of a node (for target=_blank checks) */
static HtmlNode *a_ancestor(HtmlPage *pg, int elem) {
    for (int d = 0; d < 32 && elem >= 0; d++) {
        HtmlNode *n = html2_node(pg, elem);
        if (!n) return NULL;
        if (!strcmp(n->tag, "a") && n->href && n->href[0]) return n;
        elem = n->parent;
    }
    return NULL;
}

/* is this element inside a <form>? (ancestor walk) */
static bool in_form(HtmlPage *pg, int elem) {
    for (int d = 0; d < 32 && elem >= 0; d++) {
        HtmlNode *n = html2_node(pg, elem);
        if (!n) return false;
        if (!strcmp(n->tag, "form")) return true;
        elem = n->parent;
    }
    return false;
}

/* follow a link url honoring target=_blank (opens a new tab) */
static void follow_link(VoyageWebCtx *c, Tab *t, HtmlPage *pg, int elem,
                        const char *href) {
    char full[600];
    url_resolve_base(t->url, href, full, sizeof(full));
    if (!full[0]) return;
    HtmlNode *a = a_ancestor(pg, elem);
    const char *target = a ? html2_attr(a, "target") : NULL;
    if (target && !strcasecmp(target, "_blank")) {
        tab_new(c);
        tab_nav(tab(c), full);
    } else {
        tab_nav(t, full);
    }
}

static void fire_click(VoyageWebCtx *c, Tab *t, int prim_idx) {
    HtmlPage *pg = t->page;
    if (!pg || prim_idx < 0) return;
    HtmlPrim *p = &pg->prims[prim_idx];
    if (p->elem < 0) return;
    html2_dispatch(pg, p->elem, "click", 0, 0);
    if (html2_js_cancelled(pg)) return;
    /* form controls never navigate — a widget inside <a> still belongs to
     * the page, not the link (this was stealing textbox clicks). EXCEPT
     * button-like controls: <a href="/games"><button>Games</button></a>
     * must navigate like a real link. */
    if (p->type == RP_WIDGET) {
        HtmlNode *n = p->elem >= 0 ? html2_node(pg, p->elem) : NULL;
        bool linky = false;
        if (n) {
            if (!strcmp(n->tag, "button")) linky = true;
            else if (!strcmp(n->tag, "input")) {
                const char *ty = n->type_a ? n->type_a : "text";
                linky = !strcasecmp(ty, "submit") || !strcasecmp(ty, "button") ||
                       !strcasecmp(ty, "reset") || !strcasecmp(ty, "image");
            }
        }
        /* a control inside a <form> always does its own job (submit /
         * reset / onclick) — the ancestor link must NOT steal the click.
         * Only button-like controls outside forms follow <a> wrappers. */
        if (linky && !in_form(pg, p->elem) &&
            p->link >= 0 && p->link < pg->nurls)
            follow_link(c, t, pg, p->elem, pg->urls[p->link]);
        return;
    }
    if (p->link >= 0 && p->link < pg->nurls)
        follow_link(c, t, pg, p->elem, pg->urls[p->link]);
}

static void voyageweb_event(void *ctx, GuiEvent *ev) {
    VoyageWebCtx *c = (VoyageWebCtx *)ctx;
    Tab *t = tab(c);
    int W = app_content_w();

    if (ev->type == EV_MOUSE_MOVE) {
        /* <select> dropdown hover highlight */
        if (c->sel_open >= 0) {
            int r = sel_row_at(c, ev->x, ev->y);
            if (r != c->sel_hover) { c->sel_hover = r; gui_request_redraw(); }
        }
        /* text drag: url bar or page widget */
        if (c->drag_kind == 1) {
            int ty = toolbar_y();
            int ux = ux0_now(c);
            int uw = W - ux - 10;
            int tx0 = url_text_x(t, ux, uw);
            t->url_caret = url_char_at(t, ev->x, tx0);
            return;
        }
        if (c->drag_kind == 2 && t->page && c->drag_prim >= 0 &&
            c->drag_prim < t->page->nprim) {
            HtmlPrim *p = &t->page->prims[c->drag_prim];
            int rel = (ev->x - PAD_L) - p->x;
            html2_widget_drag(t->page, p->elem, rel);
            return;
        }
        if (c->sbar >= 0) {
            int vh = view_h_now();
            int cy0 = cy0_now();
            int page_h = t->page ? t->page->page_h : 0;
            int maxsc = page_h - vh; if (maxsc < 0) maxsc = 0;
            int th = vh - 8;
            int thumb_h = th * vh / (page_h + 1); if (thumb_h < 20) thumb_h = 20;
            int travel = th - thumb_h;
            if (travel > 0 && maxsc > 0) {
                int ns = (ev->y - cy0 - 4 - c->sbar) * maxsc / travel;
                if (ns < 0) ns = 0;
                if (ns > maxsc) ns = maxsc;
                t->scroll = ns;
            }
            return;
        }
        /* tab hover */
        c->hov_tab = tab_hit(c, ev->x, ev->y);
        c->hov_ui = voyageweb_ui_hit(c, ev->x, ev->y);
        int px = 0, py = 0;
        int hp = page_hit(c, t, ev->x, ev->y, &px, &py);
        c->hov_prim = hp;
        int cur = -1;
        if (c->hov_ui == 4) cur = CUR_IBEAM;
        else if (c->hov_ui) cur = CUR_HAND;
        else if (hp >= 0 && t->page) {
            HtmlPrim *p = &t->page->prims[hp];
            if (p->link >= 0) cur = CUR_HAND;
            else if (p->type == RP_WIDGET) {
                HtmlNode *n = html2_node(t->page, p->elem);
                const char *ty = n && n->type_a ? n->type_a : "";
                if (p->wkind == WK_TEXT || p->wkind == WK_TEXTAREA ||
                    p->wkind == WK_SELECT || !strcasecmp(ty, "text"))
                    cur = CUR_IBEAM;
                else cur = CUR_HAND;
            }
        }
        gui_set_cursor(cur);
        return;
    }

    if (ev->type == EV_MOUSE_UP) {
        int was_ui = c->prs_ui;
        c->prs_ui = 0; c->sbar = -1;
        c->drag_kind = 0; c->drag_prim = -1;
        if (was_ui == 1) { tab_go_back(t); return; }
        if (was_ui == 2) { tab_go_fwd(t); return; }
        if (was_ui == 3) { voyageweb_fetch(t); return; }
        int pi = c->prs_prim; c->prs_prim = -1;
        if (pi >= 0 && t->page) {
            int px = 0, py = 0;
            int hp = page_hit(c, t, ev->x, ev->y, &px, &py);
            if (hp == pi) fire_click(c, t, pi);
        }
        return;
    }

    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        /* open <select> dropdown captures clicks in the content area:
         * a row picks an option, anywhere else just closes it */
        if (c->sel_open >= 0 && t->page && ev->y >= TAB_H) {
            int r = sel_row_at(c, ev->x, ev->y);
            if (r >= 0) {
                int opts[SEL_MAX_SHOW];
                int n = html2_select_options(t->page, c->sel_open, opts, SEL_MAX_SHOW);
                if (r < n) html2_select_choose(t->page, opts[r]);
                sel_close(c);
                gui_request_redraw();
                return;
            }
            sel_close(c);
            if (ev->y >= cy0_now()) { gui_request_redraw(); return; }
        }
        /* tab strip */
        if (ev->y < TAB_H) {
            int ti = tab_hit(c, ev->x, ev->y);
            if (ti == -2) { tab_new(c); t = tab(c); return; }
            if (ti == -3) { tab_scroll_by(c, -1); return; }
            if (ti == -4) { tab_scroll_by(c, +1); return; }
            if (ti >= 0) {
                if (tab_close_hit(c, ev->x, ev->y, ti)) { tab_close(c, ti); t = tab(c); sel_close(c); return; }
                voyageweb_set_active(c, ti); t = tab(c); sel_close(c);
                return;
            }
            return;
        }
        /* scrollbar */
        int vh = view_h_now();
        int cy0 = cy0_now();
        int page_h = t->page ? t->page->page_h : 0;
        int maxsc = page_h - vh; if (maxsc < 0) maxsc = 0;
        int th = vh - 8;
        int thumb_h = th * vh / (page_h + 1); if (thumb_h < 20) thumb_h = 20;
        int travel = th - thumb_h;
        int ty = cy0 + 4;
        int thumb_y = ty + (maxsc > 0 && travel > 0 ? travel * t->scroll / maxsc : 0);
        if (maxsc > 0 && ev->x >= W - 12 && ev->x < W - 4 &&
            ev->y >= ty && ev->y < ty + th) {
            if (ev->y >= thumb_y && ev->y < thumb_y + thumb_h)
                c->sbar = ev->y - thumb_y;
            else {
                int page = th * 3 / 4;
                t->scroll += (ev->y < thumb_y) ? -page : page;
                if (t->scroll < 0) t->scroll = 0;
                if (t->scroll > maxsc) t->scroll = maxsc;
            }
            return;
        }
        c->prs_ui = voyageweb_ui_hit(c, ev->x, ev->y);
        if (c->prs_ui == 4) {
            /* url field: focus + place caret + start drag-select */
            t->url_focused = true;
            if (t->page) t->page->focus_elem = -1;
            int ux = ux0_now(c);
            int uw = W - ux - 10;
            int tx0 = url_text_x(t, ux, uw);
            t->url_caret = url_char_at(t, ev->x, tx0);
            t->url_sel_a = t->url_caret;
            c->drag_kind = 1;
            return;
        }
        if (c->prs_ui) { c->prs_t = gui_ticks(); return; }

        t->url_focused = false;
        if (t->page) t->page->focus_elem = -1;
        int px = 0, py = 0;
        int hp = page_hit(c, t, ev->x, ev->y, &px, &py);
        c->prs_prim = hp;
        if (hp < 0 || !t->page) return;

        HtmlPrim *p = &t->page->prims[hp];
        if (p->type == RP_WIDGET && p->elem >= 0) {
            HtmlNode *n = html2_node(t->page, p->elem);
            const char *ty = n && n->type_a ? n->type_a : "";
            if (p->wkind == WK_SELECT) {
                /* dropdown, not a text field: toggle the option list */
                if (c->sel_open == p->elem) sel_close(c);
                else {
                    c->sel_open = p->elem;
                    c->sel_x = PAD_L + p->x;
                    c->sel_y = cy0_now() + p->y - t->scroll;
                    c->sel_w = p->w; c->sel_h = p->h;
                    c->sel_hover = -1;
                }
                t->page->focus_elem = p->elem;   /* keeps up/down key cycling */
                gui_request_redraw();
                return;
            }
            bool editable = p->wkind == WK_TEXT || p->wkind == WK_TEXTAREA ||
                            !strcasecmp(ty, "text") || !strcasecmp(ty, "password") ||
                            !strcasecmp(ty, "search") || !strcasecmp(ty, "email") ||
                            !strcasecmp(ty, "url") || !strcasecmp(ty, "tel") ||
                            !strcasecmp(ty, "number");
            if (editable) {
                t->page->focus_elem = p->elem;
                html2_widget_click(t->page, p->elem, px - p->x);
                c->drag_kind = 2;
                c->drag_prim = hp;
                gui_set_cursor(CUR_IBEAM);
                return;
            }
            t->page->focus_elem = -1;
            return;
        }
        t->page->focus_elem = -1;
        return;
    }

    if (ev->type == EV_MOUSE_WHEEL) {
        int vh = view_h_now();
        t->scroll -= ev->wheel * 40;
        int page_h = t->page ? t->page->page_h : 0;
        int maxsc = page_h - vh; if (maxsc < 0) maxsc = 0;
        if (t->scroll < 0) t->scroll = 0;
        if (t->scroll > maxsc) t->scroll = maxsc;
        (void)W;
        return;
    }

    if (ev->type == EV_KEY_CHAR) {
        if (t->url_focused) {
            if (ev->key == KEY_ENTER || ev->key == 13) {
                char norm[600];
                if (url_normalize(t->url_edit, norm, sizeof(norm))) tab_nav(t, norm);
                else snprintf(t->status, sizeof(t->status), "Bad address");
                return;
            }
            if (ev->key == KEY_BACKSPACE || ev->key == 8 || ev->key == 127) {
                url_backspace(t);
                return;
            }
            if (ev->key == KEY_ESC) { t->url_focused = false; return; }
            if (ev->key >= 32 && ev->key < 127) url_insert_ch(t, (char)ev->key);
            return;
        }
        /* zoom keys: +/= zoom in, - zoom out (only when no field has
         * focus, so typing them in a textbox still works) */
        if (t->page && (ev->key == '+' || ev->key == '=' || ev->key == '-')) {
            if (!(t->page->focus_elem >= 0)) {
                /* throttle: one relayout per 120ms max - holding the key
                 * (or tapping fast) no longer stalls the whole GUI loop
                 * and starves the audio DAC */
                uint64_t now = gui_ticks();
                if (now - t->last_zoom_t < 120) return;
                t->last_zoom_t = now;
                int nz = t->zoom + (ev->key == '-' ? -10 : 10);
                if (nz < 50) nz = 50;
                if (nz > 200) nz = 200;
                if (nz != t->zoom) {
                    t->zoom = nz;
                    html2_set_zoom(t->page, nz);
                    snprintf(t->status, sizeof(t->status), "Zoom %d%%", nz);
                }
                return;
            }
        }
        /* backspace/delete arrive as CHAR events (key 8 / 127) from the
         * PS/2 driver — handle them for the focused page widget */
        if (t->page && t->page->focus_elem >= 0 &&
            (ev->key == 8 || ev->key == 127 || ev->key == KEY_BACKSPACE)) {
            html2_widget_backspace(t->page, t->page->focus_elem);
            return;
        }
        if (t->page && t->page->focus_elem >= 0 && ev->key >= 32 && ev->key < 127) {
            HtmlNode *fn = html2_node(t->page, t->page->focus_elem);
            if (!fn || strcmp(fn->tag, "select"))   /* selects don't take text */
                html2_widget_insert(t->page, t->page->focus_elem, (char)ev->key);
            return;
        }
        if (t->page && t->page->focus_elem >= 0 && (ev->key == KEY_ENTER || ev->key == 13)) {
            html2_widget_enter(t->page, t->page->focus_elem);
            return;
        }
        /* Enter with nothing focused: activate the hovered link */
        if ((ev->key == KEY_ENTER || ev->key == 13) && t->page &&
            t->state == ST_DONE && c->hov_prim >= 0 &&
            c->hov_prim < t->page->nprim) {
            HtmlPrim *hp = &t->page->prims[c->hov_prim];
            if (hp->link >= 0) { fire_click(c, t, c->hov_prim); return; }
        }
        return;
    }

    if (ev->type == EV_KEY_SPECIAL) {
        if (t->url_focused) {
            int k = ev->key;
            if (k == KEY_BACKSPACE || k == 8 || k == 127) { url_backspace(t); return; }
            if (k == KEY_DELETE) { url_delete(t); return; }
            if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_HOME || k == KEY_END) {
                url_key(t, k);
                return;
            }
            if (k == KEY_ESC) { t->url_focused = false; return; }
        }
        if (t->page && t->page->focus_elem >= 0) {
            int k = ev->key;
            if (k == KEY_BACKSPACE || k == 8) {
                html2_widget_backspace(t->page, t->page->focus_elem);
                return;
            }
            if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_HOME || k == KEY_END ||
                k == KEY_UP || k == KEY_DOWN) {
                int hk = k == KEY_LEFT ? 37 : k == KEY_RIGHT ? 39 :
                         k == KEY_HOME ? 36 : k == KEY_END ? 35 :
                         k == KEY_UP ? 38 : 40;
                html2_widget_key(t->page, t->page->focus_elem, hk);
                return;
            }
            if (k == KEY_DELETE) {
                html2_widget_key(t->page, t->page->focus_elem, 46);
                return;
            }
        }
        if (ev->key == KEY_ESC) {
            if (c->sel_open >= 0) { sel_close(c); gui_request_redraw(); return; }
            t->url_focused = false;
            if (t->page) t->page->focus_elem = -1;
        }
        return;
    }
}

static void voyageweb_update(void *ctx) {
    VoyageWebCtx *c = (VoyageWebCtx *)ctx;
    for (int i = 0; i < c->ntabs; i++) {
        Tab *t = c->tabs[i];
        if (t->need_load) {
            t->need_load = false;
            voyageweb_fetch(t);
            gui_request_redraw();
            continue;
        }
        if (t->page) {
            html2_js_tick(t->page, gui_ticks());
            char nav[600];
            if (html2_js_take_nav(t->page, nav, sizeof(nav))) {
                /* JS location.href and form actions can be relative —
                 * resolve against the current page URL or the address
                 * bar ends up showing garbage like "???" */
                char full[640];
                url_resolve_base(t->url, nav, full, sizeof(full));
                if (full[0]) tab_nav(t, full);
                else if (nav[0] == '?') tab_nav(t, t->url); /* bare query: reload */
                gui_request_redraw();
                continue;
            }
            if (t->page->dirty) {
                /* g_cur_win is NULL outside draw/event dispatch, so
                 * pw_now() collapses to the 200px floor here - relayout
                 * at that width, stamp last_w=200, and the next draw
                 * relayouts back to the real width: the page ping-pongs
                 * ("shrinks up and down") on every dirty flag. Use the
                 * width from the last real draw; draw() owns width
                 * changes via its own last_w check. */
                int pw = t->last_w > 0 ? t->last_w : 800;
                int old_h = t->page->page_h, os = t->scroll;
                html2_relayout(t->page, pw);
                int nh = t->page->page_h;
                if (old_h > 0 && nh > 0 && nh != old_h)
                    t->scroll = (int)((int64_t)os * nh / old_h);
            }
        }
        if (t->state == ST_LOADING) gui_request_redraw();
    }
    /* caret blink + anim */
    if (((gui_ticks() / 500) & 1) || c->sbar >= 0 || c->drag_kind)
        gui_request_redraw();
}

static App s_app = {
    "Voyage Web", "Voyage Web", 720, 520, -1,
    voyageweb_create, voyageweb_destroy, voyageweb_init, voyageweb_draw, voyageweb_event,
    voyageweb_update, 0, 0
};
App *make_voyageweb(void) { return &s_app; }
