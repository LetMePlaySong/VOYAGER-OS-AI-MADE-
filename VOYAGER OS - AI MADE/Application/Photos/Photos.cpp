/* Preview.app - direct .png viewer. Double-clicking any PNG opens this
 * straight onto the image; < > buttons (or the wheel) flip through the
 * other .png files in the SAME directory. No browsing grid - it is a
 * viewer, not a photo library. Works for user VFS images (modifiable)
 * and System HD ISO images (view-only). */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../gui/gui.h"
#include "../../kernel/vfs.h"
#include "../../kernel/sysvol.h"
#include "../../kernel/fb.h"
#include "../../kernel/k.h"

#define MAX_IMGS 64

typedef struct {
    int  kind;                     /* 0 = user VFS, 1 = System HD (ISO) */
    char dir[VFS_NAME_LEN + 1];    /* VFS folder ("" = root) - kind 0 */
    char name[VFS_NAME_LEN + 1];   /* file name - kind 0 */
    char path[128];                /* full ISO path - kind 1 */
} Img;

typedef struct {
    Img  imgs[MAX_IMGS];
    int  count;
    int  cur;                      /* index of the image being viewed */
    int  hov;                      /* -1 none, -3 prev, -4 next */
    int  prs;                      /* pressed nav button, same ids */
    int  empty;                    /* launched without a pending image */
} PrevCtx;

static bool is_png_name(const char *n) { return gui_is_png(n); }

static void add_vfs(PrevCtx *c, const char *dir, const char *name) {
    if (c->count >= MAX_IMGS) return;
    Img *it = &c->imgs[c->count++];
    memset(it, 0, sizeof(*it));
    it->kind = 0;
    strncpy(it->dir, dir, sizeof(it->dir) - 1);
    strncpy(it->name, name, sizeof(it->name) - 1);
}

static void add_sys(PrevCtx *c, const char *path) {
    if (c->count >= MAX_IMGS) return;
    Img *it = &c->imgs[c->count++];
    memset(it, 0, sizeof(*it));
    it->kind = 1;
    strncpy(it->path, path, sizeof(it->path) - 1);
    const char *b = strrchr(path, '/');
    strncpy(it->name, b ? b + 1 : path, sizeof(it->name) - 1);
}

/* enumerate every .png that lives in the same directory as the pending
 * image, so < > can flip through them like a folder of photos */
static void build_siblings(PrevCtx *c, const char *dir, const char *name) {
    c->count = 0;
    if (dir[0] == '/') {
        /* System HD: dir is an ISO path like "/boot/assets" (leading '/'
         * marks sysvol; sysvol itself stores paths without it) */
        const char *sd = dir + 1;
        int n = sysvol_count(sd);
        for (int i = 0; i < n; i++) {
            const VfsEntry *e = sysvol_entry(sd, i);
            if (!e || e->type != VFS_FILE || !is_png_name(e->name))
                continue;
            char fp[160];
            snprintf(fp, sizeof(fp), "%s%s%s", sd,
                     sd[0] ? "/" : "", e->name);
            const SysVolEnt *se = sysvol_lookup(fp);
            if (se && se->has_blob) add_sys(c, fp);
        }
        for (int i = 0; i < c->count; i++) {
            const char *b = strrchr(c->imgs[i].path, '/');
            const char *bn = b ? b + 1 : c->imgs[i].path;
            if (!strcmp(bn, name)) { c->cur = i; return; }
        }
        c->cur = 0;
        return;
    }
    /* user VFS: dir "" = desktop root, else a folder name */
    int n = dir[0] ? vfs_count_in(dir) : vfs_count();
    for (int i = 0; i < n; i++) {
        const VfsEntry *e = dir[0] ? vfs_entry_in(dir, i) : vfs_entry(i);
        if (e && e->type == VFS_FILE && !e->system && is_png_name(e->name))
            add_vfs(c, dir, e->name);
    }
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->imgs[i].name, name)) { c->cur = i; return; }
    c->cur = 0;
}

static const uint8_t *img_pixels(PrevCtx *c, Img *it,
                                 uint32_t *w, uint32_t *h) {
    if (it->kind == 1) return gui_sysvol_image(it->path, w, h);
    return gui_vfs_image(it->dir, it->name, w, h);
}

static void *prev_create(void) {
    PrevCtx *c = (PrevCtx *)kzalloc(sizeof(PrevCtx));
    c->cur = -1; c->hov = -1; c->prs = -1;
    const char *ps = gui_pending_sysimg();
    if (ps) {
        /* ISO path -> siblings live in the same ISO directory */
        char dir[128];
        const char *sl = strrchr(ps, '/');
        if (sl) {
            int dl = (int)(sl - ps); if (dl > 127) dl = 127;
            memcpy(dir, ps, dl); dir[dl] = 0;
        } else dir[0] = 0;
        const char *nm = sl ? sl + 1 : ps;
        /* mark sysvol dirs with a leading '/' so build_siblings can tell
         * them apart from VFS folder names (which never contain '/') */
        char dkey[130];
        snprintf(dkey, sizeof(dkey), "/%s", dir);
        build_siblings(c, dkey, nm);
        gui_clear_pending_sysimg();
        return c;
    }
    const char *pi = gui_pending_image();
    if (pi) {
        char dir[80], nm[48];
        const char *sl = strchr(pi, '/');
        if (sl) {
            int dl = (int)(sl - pi); if (dl > 79) dl = 79;
            memcpy(dir, pi, dl); dir[dl] = 0;
            strncpy(nm, sl + 1, sizeof(nm) - 1); nm[sizeof(nm)-1] = 0;
        } else {
            dir[0] = 0;
            strncpy(nm, pi, sizeof(nm) - 1); nm[sizeof(nm)-1] = 0;
        }
        build_siblings(c, dir, nm);
        gui_clear_pending_image();
        return c;
    }
    /* launched with no image (e.g. stray icon): show a hint instead */
    c->empty = 1;
    return c;
}
static void prev_destroy(void *ctx) { kfree(ctx); }
static void prev_init(void *ctx) { (void)ctx; }

static int nav_hit(int x, int y) {
    int W = app_content_w(), H = app_content_h();
    int py = 40 + (H - 52) / 2 - 13;
    if (x >= 10 && x < 44 && y >= py && y < py + 26) return -3;   /* < */
    int nx = W - 44;
    if (x >= nx && x < nx + 34 && y >= py && y < py + 26) return -4; /* > */
    return 0;
}

static void prev_draw(void *ctx) {
    PrevCtx *c = (PrevCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(31, 14, 34));

    if (c->empty || c->count == 0 || c->cur < 0) {
        app_text(20, 20, "Photo Viewer", rgb(233, 200, 240), 2);
        app_text(20, 60, "Double-click a .png file to view it.",
                 rgb(201, 150, 210), 1);
        return;
    }

    /* < > navigation (only when the directory has more than one image) */
    if (c->count > 1) {
        int py = 40 + (H - 52) / 2 - 13;
        bool hp = (c->hov == -3), hn = (c->hov == -4);
        bool pp = (c->prs == -3), pn = (c->prs == -4);
        app_fill(10, py, 34, 26,
                 pp ? rgb(50, 24, 55) : hp ? rgb(96, 52, 105) : rgb(61, 30, 66));
        app_rect(10, py, 34, 26, pp ? rgb(127, 70, 140) : rgb(182, 96, 200));
        app_text(20, py + 8, "<", rgb(249, 232, 255), 1);
        int nx = W - 44;
        app_fill(nx, py, 34, 26,
                 pn ? rgb(50, 24, 55) : hn ? rgb(96, 52, 105) : rgb(61, 30, 66));
        app_rect(nx, py, 34, 26, pn ? rgb(127, 70, 140) : rgb(182, 96, 200));
        app_text(nx + 10, py + 8, ">", rgb(249, 232, 255), 1);
    }
    Img *it = &c->imgs[c->cur];
    char cap[120];
    if (c->count > 1)
        snprintf(cap, sizeof(cap), "%s  (%d/%d)%s", it->name,
                 c->cur + 1, c->count,
                 it->kind == 1 ? "  [System HD - read only]" : "");
    else
        snprintf(cap, sizeof(cap), "%s%s", it->name,
                 it->kind == 1 ? "  [System HD - read only]" : "");
    app_text_clip(W / 2 - app_text_width(cap, 1) / 2, 12, cap,
                  rgb(231, 190, 240), 1, 0, 0, W, 30);

    uint32_t iw = 0, ih = 0;
    const uint8_t *img = img_pixels(c, it, &iw, &ih);
    if (img) {
        int aw = W - 24 - (c->count > 1 ? 88 : 0);
        int ah = H - 52;
        int dw = aw, dh = ah;
        if (iw && ih) {
            if ((int64_t)dw * ih > (int64_t)dh * iw)
                dw = (int)(dh * (int64_t)iw / ih);
            else
                dh = (int)(dw * (int64_t)ih / iw);
        }
        int ox = (c->count > 1 ? 44 : 12) + (aw - dw) / 2;
        app_draw_image_scaled(ox, 40 + (ah - dh) / 2,
                              dw, dh, img, (int)iw, (int)ih);
    } else {
        app_text(W / 2 - 90, H / 2, "Can't decode this image",
                 rgb(201, 150, 210), 1);
    }
}

static void prev_event(void *ctx, GuiEvent *ev) {
    PrevCtx *c = (PrevCtx *)ctx;
    if (c->empty || c->count == 0) return;
    if (ev->type == EV_MOUSE_MOVE) {
        c->hov = nav_hit(ev->x, ev->y);
        gui_set_cursor(c->hov ? CUR_HAND : -1);
        return;
    }
    if (ev->type == EV_MOUSE_WHEEL && c->count > 1) {
        int d = ev->wheel > 0 ? 1 : -1;
        c->cur = (c->cur + d + c->count) % c->count;
        return;
    }
    if (ev->type == EV_MOUSE_UP) { c->prs = -1; return; }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int n = nav_hit(ev->x, ev->y);
        c->prs = n;
        if (n == -3) { c->cur = (c->cur + c->count - 1) % c->count; return; }
        if (n == -4) { c->cur = (c->cur + 1) % c->count; return; }
    }
}
static void prev_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Photos", "Photo Viewer", 720, 540, -1,
    prev_create, prev_destroy, prev_init, prev_draw,
    prev_event, prev_update, 0, 0
};
App *make_photos(void) { return &s_app; }
