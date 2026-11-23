/* Finder.app - file browser showing System HD + Applications */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../gui/gui.h"
#include "../../kernel/vfs.h"
#include "../../kernel/sysvol.h"
#include "../../drivers/ata.h"

/* unregistered .png viewer spawned directly on double-click */
extern "C" App *app_make_photos(void);

typedef struct {
    int sel;   /* selected icon index, -1 none, -2 = System HD */
    int hov, prs; /* hovered / pressed icon for cursor+feedback */
    int hov_side, prs_side; /* sidebar hover/press: 0-3 favs, 10+i disks,
                               20/21 = +Folder/+File buttons, -1 none */
    int fav;      /* selected sidebar favorite: 0=System HD 1=Applications 2=Users 3=Documents */
    char folder[VFS_NAME_LEN + 1];  /* "" = normal view, else inside this folder */
    int drive;                      /* 1 = browsing the System HD volume (ISO) */
    int dev;                        /* >=0 = viewing detected drive #dev */
    int devdir;                     /* dir slot inside foreign volume, -1 = root */
    char sysdir[SYSVOL_PATH];       /* path inside the volume, "" = root */
    uint64_t prs_t;
    /* drag & drop state (Win10-style move into/out of folders) */
    int mx, my;                 /* last mouse pos (content coords) */
    int prs_x, prs_y;           /* press position (drag threshold) */
    int drag_id;                /* icon being dragged, -1 none */
    int drag_ox, drag_oy;       /* grab offset inside the icon */
    int dragging;
    int drag_type;                          /* VFS_DIR / VFS_FILE */
    int drag_app;                           /* dragging a built-in app icon */
    char drag_name[VFS_NAME_LEN + 1];
    char drag_src[VFS_NAME_LEN + 1];   /* "" = root view */
    int drag_fx;                /* foreign-volume slot being dragged, -1 = VFS/app */
    int scroll;                         /* browse grid scroll offset (px) */
    int sbar;                           /* scrollbar grab offset, -1 = not dragging */
    /* in-app context menu: right-click item -> Rename / Delete;
     * right-click a disk -> Eject / Refresh / Properties (menu_dev) */
    int menu_x, menu_y, menu_open, menu_hit, menu_dev;
    int fav_ay;                 /* highlight top the selection slid FROM */
    uint64_t fav_anim_t;        /* nonzero while the sidebar pill glides */
    int side_from_y;            /* unified highlight: pixel y it slid FROM */
    int side_last_y;            /* unified highlight: last drawn y (-1 none) */
    uint64_t side_anim_t;       /* nonzero while the unified pill glides */
    /* transient "This action is denied!" banner */
    char msg[64];
    uint64_t msg_t;
    int drag_sys;   /* dragged entry is a protected system item */
    int last_id;    /* last clicked icon (double-click open tracking) */
    uint64_t last_ms;
} FinderCtx;

/* root entries split in two: user files (normal view) vs system volume
 * (only visible inside the System HD drive view) */
static int root_user_count(void) {
    int n = 0, vc = vfs_count();
    for (int i = 0; i < vc; i++) {
        const VfsEntry *e = vfs_entry(i);
        if (e && !e->system) n++;
    }
    return n;
}
static const VfsEntry *root_user(int j) {
    int vc = vfs_count();
    for (int i = 0; i < vc; i++) {
        const VfsEntry *e = vfs_entry(i);
        if (e && !e->system && j-- == 0) return e;
    }
    return NULL;
}
/* unified browse source: drive = real ISO system volume, folder = its
 * children, otherwise the normal root view (user files only) */
static int browse_count(FinderCtx *c) {
    if (c->drive) return sysvol_count(c->sysdir);
    return c->folder[0] ? vfs_count_in(c->folder) : root_user_count();
}
static const VfsEntry *browse_entry(FinderCtx *c, int i) {
    if (c->drive) return sysvol_entry(c->sysdir, i);
    return c->folder[0] ? vfs_entry_in(c->folder, i) : root_user(i);
}
/* is the folder we're browsing a protected system dir? (drive root = no) */
static int browse_system(FinderCtx *c) {
    if (c->drive || !c->folder[0]) return 0;
    int fi = vfs_find(c->folder);
    const VfsEntry *fe = (fi >= 0) ? vfs_entry(fi) : NULL;
    return fe && fe->system;
}
static void msg_set(FinderCtx *c, const char *m) {
    strncpy(c->msg, m, sizeof(c->msg) - 1);
    c->msg[sizeof(c->msg) - 1] = 0;
    c->msg_t = gui_ticks();
}
static void deny(FinderCtx *c) { msg_set(c, "This action is denied!"); gui_alert_beep(); }

/* ".lnk" shortcut files launch built-in apps instead of opening in Quill.
 * Content = app name. Returns 1 if the file was a shortcut and launched. */
static int open_lnk(const char *dir, const char *name) {
    int nl = (int)strlen(name);
    if (nl < 5 || strcmp(name + nl - 4, ".lnk")) return 0;
    char buf[64];
    int n = (dir && dir[0]) ? vfs_read_in(dir, name, buf, sizeof(buf) - 1)
                            : vfs_read(name, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
    App *a = gui_find_app(buf);
    if (!a) return 0;
    gui_open_app_new(a);
    return 1;
}
/* create/refresh a ".lnk" shortcut to app `appname` inside folder `dir` */
static void make_lnk(const char *dir, const char *appname) {
    char nm[VFS_NAME_LEN + 1];
    snprintf(nm, sizeof(nm), "%s.lnk", appname);
    if (nm[VFS_NAME_LEN]) nm[VFS_NAME_LEN] = 0;   /* clamp to VFS limit */
    if (vfs_create_in(nm, VFS_FILE, dir && dir[0] ? dir : NULL) != 0 &&
        vfs_find_in(dir, nm) < 0)
        return;                                   /* can't create: give up */
    vfs_write_in(dir, nm, appname, (int)strlen(appname));
}

/* New Folder / New File buttons in the folder view (content coords) */
#define FB_W 78
static int fb_hit(int x, int y, int W) {
    if (y < 6 || y >= 34) return -1;
    int bx0 = W - 2 * FB_W - 20, bx1 = bx0 + FB_W + 12;
    if (x >= bx0 && x < bx0 + FB_W) return 300;   /* +Folder */
    if (x >= bx1 && x < bx1 + FB_W) return 301;   /* +File */
    return -1;
}
static void fb_create(FinderCtx *c, int type) {
    static const char *bases[] = { "New Folder", "New File.txt" };
    char name[40];
    for (int n = 1; n < 200; n++) {
        if (n == 1) snprintf(name, sizeof(name), "%s", bases[type]);
        else snprintf(name, sizeof(name), "%s %d", bases[type], n);
        if (vfs_find_in(c->folder, name) < 0 &&
            vfs_create_in(name, type == 0 ? VFS_DIR : VFS_FILE,
                          c->folder[0] ? c->folder : NULL) == 0)
            break;
    }
}

/* live Finder windows (for the "directory in use" delete guard) */
static FinderCtx *g_finders[8];
static int g_finders_n;

/* is root folder `name` open in any Files window, or an ancestor of an
 * open folder? -> deleting it must be refused with "in use" */
int finder_folder_in_use(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_finders_n; i++) {
        FinderCtx *c = g_finders[i];
        if (!c || !c->folder[0] || c->drive) continue;
        int slot = vfs_find(c->folder);
        if (slot < 0) continue;
        for (int s = slot, depth = 0; s >= 0 && depth < 16; depth++) {
            const VfsEntry *e = vfs_entry(s);
            if (!e) break;
            if (!strcasecmp(e->name, name)) return 1;
            s = e->parent;
        }
    }
    return 0;
}

static void *find_create(void) {
    FinderCtx *c = (FinderCtx *)kzalloc(sizeof(FinderCtx));
    if (g_finders_n < 8) g_finders[g_finders_n++] = c;
    c->sel = -1; c->hov = -1; c->prs = -1; c->fav = 2; c->drive = 0; c->dev = -1;
    c->hov_side = c->prs_side = -1;
    c->devdir = -1; c->drag_fx = -1; c->menu_dev = -1;
    c->last_id = -1; c->last_ms = 0;
    c->drag_id = -1; c->dragging = 0; c->mx = c->my = 0; c->sbar = -1;
    /* opened by double-clicking a desktop folder? start inside it */
    const char *pf = gui_pending_folder();
    if (pf) {
        strncpy(c->folder, pf, sizeof(c->folder) - 1);
        c->folder[sizeof(c->folder) - 1] = 0;
        gui_clear_pending_folder();
    }
    return c;
}
static void find_init(void *ctx) { (void)ctx; }
static void find_destroy(void *ctx) {
    for (int i = 0; i < g_finders_n; i++)
        if (g_finders[i] == ctx) {
            g_finders[i] = g_finders[--g_finders_n];
            break;
        }
    kfree(ctx);
}
static int grid_cols(void);
static int grid_rows(int cells, int cols);
static int grid_px(void);
static int grid_py(int rows);
static void sbar_geom(FinderCtx *c, int vc, int *ty, int *th,
                      int *thumb_y, int *thumb_h, int *maxsc);
static void fbar_geom(FinderCtx *c, int vc, int *ty, int *th,
                      int *thumb_y, int *thumb_h, int *maxsc);

static void drive_entry_path(FinderCtx *c, int idx, char *buf, size_t cap);

/* grid icon label: wraps onto a SECOND line at a word boundary instead
 * of clipping with "..." - long filenames (the music tracks!) stay
 * readable. Line 2 still ellipsizes if the name is enormous. */
static void grid_label(int cx, int iy, const char *name) {
    char src[160];
    snprintf(src, sizeof(src), "%s", name ? name : "");
    char l1[80], l2[80]; l2[0] = 0;
    if (app_text_width(src, 1) > 96) {
        int sp = -1, wpx = 0;
        for (int i = 0; src[i] && i < 150; i++) {
            char tc[2] = { src[i], 0 };
            int cw = app_text_width(tc, 1);
            if (wpx + cw > 96) break;
            wpx += cw;
            if (src[i] == ' ') sp = i;
        }
        if (sp > 3) {
            src[sp] = 0;
            const char *r = src + sp + 1;
            while (*r == ' ') r++;
            snprintf(l1, sizeof(l1), "%s", src);
            gui_fit_label(l2, sizeof(l2), r, 96);
        } else {
            gui_fit_label(l1, sizeof(l1), src, 96);
        }
    } else {
        snprintf(l1, sizeof(l1), "%s", src);
    }
    int lw = app_text_width(l1, 1);
    app_text(cx - lw / 2, iy + 66, l1, rgb(50, 50, 50), 1);
    if (l2[0]) {
        lw = app_text_width(l2, 1);
        app_text(cx - lw / 2, iy + 82, l2, rgb(50, 50, 50), 1);
    }
}

/* bottom status line: clipped + LOOP marquee ("escalator") - long
 * paths scroll left continuously, pause on the tail, then wrap back
 * to the start instead of overflowing the window edge */
static void status_marquee(const char *s, int x, int y, int maxw,
                           uint32_t color) {
    int tw = app_text_width(s, 1);
    int dx = x;
    if (tw > maxw && maxw > 40) {
        int travel = tw - maxw + 24;
        int cyc = travel + 120;          /* scroll, hold tail, wrap */
        int p = (int)((gui_ticks() / 35) % cyc);
        if (p <= travel) dx = x - p;
        else dx = x - travel;            /* hold, then snap back */
    }
    app_text_clip(dx, y, s, color, 1, x, y - 2, maxw, 16);
}

/* draw a real PNG preview for image files; returns false for non-images
 * (caller falls back to the themed icon) */
static bool draw_item_icon(FinderCtx *c, int ix, int iy, int sz,
                           const VfsEntry *e, int idx) {
    if (!e) return false;
    if (!gui_is_png(e->name)) return false;
    uint32_t iw = 0, ih = 0;
    const uint8_t *img = NULL;
    if (e->system) {
        /* System HD image: view-only thumbnail straight from the ISO
         * module (cached decode in the GUI layer) */
        if (!c->drive) return false;
        char fp[160];
        drive_entry_path(c, idx, fp, sizeof(fp));
        img = gui_sysvol_thumb(fp, &iw, &ih);
    } else {
        if (e->type != VFS_FILE) return false;
        const char *dir = (c->folder[0] && !c->drive) ? c->folder : "";
        img = gui_vfs_thumb(dir, e->name, &iw, &ih);
    }
    if (!img) return false;
    app_draw_image_scaled(ix, iy, sz, sz, img, (int)iw, (int)ih);
    return true;
}

static void draw_drag(FinderCtx *c, int W);
static void draw_msg(FinderCtx *c);

/* ---- unified sidebar selection highlight ----
 * ONE pill covers Favorites AND Disk rows and GLIDES between them
 * (~200ms ease-out). side id space matches prs_side: 0-3 = favorites,
 * 10+i = disk i, -1 = nothing selected. */
static int side_sel_id(FinderCtx *c) {
    if (c->dev >= 0) return 10 + c->dev;
    if (c->fav >= 0 && c->fav < 4) return c->fav;
    return -1;
}
static int side_y_of(int id) {
    if (id >= 0 && id < 4) return 27 + id * 18;      /* fav highlight top */
    if (id >= 10 && id < 15) return 131 + (id - 10) * 18;
    return -1;
}
/* call AFTER fav/dev selection fields change: starts the glide from
 * wherever the pill currently sits (mid-flight positions included) */
static void side_note_change(FinderCtx *c) {
    int from = (c->side_last_y >= 0) ? c->side_last_y
                                   : side_y_of(side_sel_id(c));
    /* if an anim is mid-flight, side_last_y already tracks the drawn
     * position, so chained clicks keep gliding smoothly */
    c->side_from_y = from;
    c->side_anim_t = gui_ticks();
    c->fav_anim_t = 0;   /* legacy fav-only anim superseded */
}
/* current highlight top y for drawing (-1 = hidden) */
static int side_cur_y(FinderCtx *c) {
    int id = side_sel_id(c);
    int ty = side_y_of(id);
    if (ty < 0) { c->side_last_y = -1; return -1; }
    if (!c->side_anim_t) { c->side_last_y = ty; return ty; }
    uint64_t el = gui_ticks() - c->side_anim_t;
    if (el >= 200) { c->side_anim_t = 0; c->side_last_y = ty; return ty; }
    int e = 100 - (int)el;
    int ee = 100 - (e * e) / 100;                    /* ease-out */
    int y = (c->side_from_y >= 0 ? c->side_from_y : ty)
            + (ty - (c->side_from_y >= 0 ? c->side_from_y : ty)) * ee / 100;
    c->side_last_y = y;
    return y;
}

/* switch sidebar favorite with the highlight GLIDING between rows
 * (~200ms ease-out) - same premium feel as the Settings/Voyage Web tabs */
static void find_set_fav(FinderCtx *c, int f) {
    if (f == c->fav) return;
    c->fav = f;
    /* NOTE: callers must side_note_change() AFTER fav AND dev are both
     * updated - side_sel_id() reads both fields */
}

static void find_draw(void *ctx) {
    FinderCtx *c = (FinderCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(235, 235, 235));

    /* sidebar */
    app_fill(0, 0, 140, H, rgb(220, 220, 220));
    app_fill(140, 0, 1, H, rgb(200, 200, 200));
    app_text(12, 12, "Favorites", rgb(100, 100, 100), 1);
    /* sidebar favorites: clickable rows; selection feedback is the
     * UNIFIED pill drawn after the Disk section (glides fav<->disk) */
    static const char *kFav[] = {"System HD", "Applications", "Documents"};
    for (int i = 0; i < 4; i++) {
        int fy = 30 + i * 18;
        if (c->prs_side == i)      /* held = darker blue press */
            app_fill(6, fy - 3, 128, 15, rgba(114, 0, 200, 80));
        else if (c->hov_side == i)      /* hover = light blue wash */
            app_fill(6, fy - 3, 128, 15, rgba(138, 0, 255, 30));
        /* the "home" favorite shows the live user name from Settings */
        const char *lbl = (i == 2) ? gui_user_name() : kFav[i < 2 ? i : i - 1];
        app_text_clip(12, fy, lbl, rgb(40, 40, 40), 1, 12, fy - 2, 120, 14);
    }

    /* Disk section: every attached drive (internal, USB, optical, VM)
     * listed under its own header - click opens the drive view directly,
     * not buried inside the user folder */
    {
        app_text(12, 116, "Disk", rgb(100, 100, 100), 1);
        int nd = ata_media_count();
        for (int i = 0; i < nd && i < 5; i++) {
            int dy = 134 + i * 18;
            if (c->prs_side == 10 + i)
                app_fill(6, dy - 3, 128, 15, rgba(114, 0, 200, 80));
            else if (c->hov_side == 10 + i)
                app_fill(6, dy - 3, 128, 15, rgba(138, 0, 255, 30));
            AtaMediaInfo mi = { 0, 0, 0, "" };
            ata_media_info(i, &mi);
            char dl[32];
            if (mi.atapi) strcpy(dl, "CD/DVD Drive");
            else if (mi.removable) strcpy(dl, "USB Drive");
            else strcpy(dl, "Disk");
            app_text_clip(12, dy, dl, rgb(40, 40, 40), 1, 12, dy - 2, 120, 14);
        }
        if (nd == 0)
            app_text_clip(12, 134, "No disks", rgb(150, 150, 155), 1,
                          12, 131, 120, 14);
    }

    /* THE sidebar selection pill - one highlight that glides between
     * Favorites rows and Disk rows (drawn last so it sits on top) */
    {
        int sy = side_cur_y(c);
        if (sy >= 0) app_fill(6, sy, 128, 15, rgba(138, 0, 255, 55));
    }

    /* detected drive info view: name/type/capacity for USB, optical
     * and VM-added disks that showed up via the ATA media poll */
    if (c->dev >= ata_media_count()) c->dev = -1;   /* disk was removed */
    if (c->dev >= 0) {
        AtaMediaInfo mi = { 0, 0, 0, "" };
        char title[64] = "Drive";
        char kind[32] = "Disk";
        char cap[32] = "";
        if (ata_media_info(c->dev, &mi)) {
            if (mi.atapi) strcpy(kind, "CD/DVD Drive");
            else if (mi.removable) strcpy(kind, "USB Drive");
            else strcpy(kind, "External Disk");
            if (mi.model[0]) {
                strncpy(title, mi.model, sizeof(title) - 1);
                title[sizeof(title) - 1] = 0;
            } else strcpy(title, kind);
            if (mi.sectors && !mi.atapi) {
                uint64_t mb = (uint64_t)mi.sectors * 512 / (1024 * 1024);
                if (mb >= 1024)
                    snprintf(cap, sizeof(cap), "%u.%u GB",
                             (unsigned)(mb / 1024), (unsigned)((mb % 1024) / 100));
                else
                    snprintf(cap, sizeof(cap), "%u MB", (unsigned)mb);
            } else if (mi.atapi) strcpy(cap, "Optical disc");
            else strcpy(cap, "Capacity unknown");
        }
        app_text(160, 14, title, rgb(50, 50, 50), 2);
        if (!vfsx_probe(c->dev)) {
            /* not a Voyager volume: show the drive card */
            app_text(160, 70, kind, rgb(90, 90, 95), 1);
            if (cap[0]) app_text(160, 96, cap, rgb(90, 90, 95), 1);
            app_text(160, 140,
                     mi.atapi || mi.removable
                         ? "This disk is connected and ready."
                         : "This disk is connected.",
                     rgb(110, 110, 115), 1);
            app_text(160, 160,
                     "This disk isn't a Voyager volume - its files "
                     "can't be browsed.",
                     rgb(140, 140, 145), 1);
            draw_drag(c, W);
            draw_msg(c);
            return;
        }
        /* Voyager volume: browse its directory directly */
        int n = vfsx_count(c->dev, c->devdir);
        int cols = grid_cols(), cell_w = grid_px();
        int rows = grid_rows(n + 1, cols), cell_h = grid_py(rows);
        int start_x = 160, start_y = 50;
        int maxsc = rows * cell_h - (H - 96);
        if (maxsc < 0) maxsc = 0;
        if (c->scroll > maxsc) c->scroll = maxsc;
        if (c->scroll < 0) c->scroll = 0;
        /* ".." parent entry (id 199), always drawn so hit geometry is
         * identical at every level */
        {
            int iy = start_y - c->scroll;
            if (iy >= 38 && iy <= H - 30) {
                if (c->sel == 199) app_fill(start_x - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
                if (c->hov == 199) app_fill(start_x - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
                asset_draw(start_x, iy, 64, "files");
                app_text(start_x + 20, iy + 66, "..", rgb(50, 50, 50), 1);
            }
        }
        for (int i = 0; i < n; i++) {
            VfsEntry e; int slot = -1;
            if (!vfsx_get(c->dev, c->devdir, i, &e, &slot)) break;
            int col = (i + 1) % cols, row = (i + 1) / cols;
            int ix = start_x + col * cell_w;
            int iy = start_y + row * cell_h - c->scroll;
            if (ix + 64 > W) break;
            if (iy < 38 || iy + 90 > H - 30) continue;
            int id = 100 + i;
            if (c->sel == id) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
            if (c->prs == id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
            } else if (c->hov == id && c->sel != id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
            }
            asset_draw(ix, iy, 64,
                       e.type == VFS_DIR ? "files" : gui_file_icon(e.name));
            grid_label(ix + 32, iy, e.name);
        }
        if (n == 0 && c->devdir < 0)
            app_text(160, 120, "This disk is empty.", rgb(110, 110, 115), 2);
        /* hint bar: how to get files on/off this disk */
        char hint[96];
        snprintf(hint, sizeof(hint), "%d item%s - double-click a file to "
                 "copy it to %s, drag files onto the Disk row to copy in",
                 n, n == 1 ? "" : "s", gui_user_name());
        status_marquee(hint, 160, H - 22, W - 160 - 16, rgb(120, 120, 125));
        draw_drag(c, W);
        draw_msg(c);
        return;
    }

    /* browse mode: inside a folder OR the System HD volume */
    if (c->folder[0] || c->drive) {
        char title[128];
        if (c->drive)
            snprintf(title, sizeof(title), "System HD%s%s",
                     c->sysdir[0] ? "/" : "", c->sysdir);
        else
            snprintf(title, sizeof(title), "%s", c->folder);
        app_text(160, 14, title, rgb(50, 50, 50), 2);
        /* New Folder / New File buttons (hidden inside the read-only
         * System HD volume - it is a disk image, not a workspace) */
        int bw = app_content_w();
        for (int b = 0; b < (c->drive ? 0 : 2); b++) {
            int bx = bw - 2 * FB_W - 20 + b * (FB_W + 12);
            bool hov = (c->mx >= bx && c->mx < bx + FB_W && c->my >= 6 && c->my < 34);
            bool prs = (c->prs_side == 20 + b);
            app_rounded(bx, 6, FB_W, 28, 6,
                        prs ? rgb(114, 0, 200) : hov ? rgb(138, 0, 255) : rgb(235, 235, 235));
            app_rect(bx, 6, FB_W, 28, rgb(200, 200, 200));
            const char *bl = b == 0 ? "+ Folder" : "+ File";
            int lw = app_text_width(bl, 1);
            app_text(bx + (FB_W - lw) / 2, 15, bl,
                     (hov || prs) ? rgb(255, 255, 255) : rgb(60, 60, 60), 1);
        }
        int vc = browse_count(c);
        int cols = grid_cols(), cell_w = grid_px();
        int rows = grid_rows(vc + 1, cols), cell_h = grid_py(rows);
        int start_x = 160, start_y = 50;
        /* clamp scroll into the valid range for this content */
        int ty = 0, th = 0, thumb_y = 0, thumb_h = 0, maxsc = 0;
        sbar_geom(c, vc, &ty, &th, &thumb_y, &thumb_h, &maxsc);
        if (c->scroll > maxsc) c->scroll = maxsc;
        if (c->scroll < 0) c->scroll = 0;
        /* ".." back entry at slot 0 -> id 199 in hit-tests; it scrolls
         * along with the grid like every other row */
        int iy0 = start_y - c->scroll;
        if (iy0 >= 38 && iy0 <= H - 30) {
            if (c->sel == 199) app_fill(start_x - 4, iy0 - 4, 72, 72, rgba(138, 0, 255, 60));
            if (c->hov == 199) app_fill(start_x - 4, iy0 - 4, 72, 72, rgba(138, 0, 255, 45));
            asset_draw(start_x, iy0, 64, "files");
            app_text(start_x + 20, iy0 + 66, "..", rgb(50, 50, 50), 1);
        }
        for (int i = 0; i < vc; i++) {
            const VfsEntry *e = browse_entry(c, i);
            if (!e) break;
            int col = (i + 1) % cols, row = (i + 1) / cols;
            int ix = start_x + col * cell_w;
            int iy = start_y + row * cell_h - c->scroll;
            if (ix + 64 > W) break;
            /* clip: icons scroll UNDER the toolbar, never draw over it */
            if (iy < 38 || iy + 90 > H - 30) continue;
            int id = 100 + i;
            if (c->sel == id) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
            if (c->prs == id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
            } else if (c->hov == id && c->sel != id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
            }
            if (!draw_item_icon(c, ix, iy, 64, e, i))
                asset_draw(ix, iy, 64, e->type == VFS_DIR ? "files" : gui_file_icon(e->name));
            grid_label(ix + 32, iy, e->name);
        }
        /* scrollbar when the grid overflows - click/drag the thumb,
         * click the track to page */
        if (maxsc > 0) {
            app_fill(W - 12, ty, 6, th, rgb(210, 210, 210));
            app_fill(W - 12, thumb_y, 6, thumb_h, rgb(150, 150, 155));
        }
        if (vc == 0) app_text(160, 120, "This folder is empty.", rgb(110, 110, 115), 2);
        /* drive view: show real file details for the selected item */
        if (c->drive && c->sel >= 100) {
            char fp[160];
            drive_entry_path(c, c->sel - 100, fp, sizeof(fp));
            const SysVolEnt *se = sysvol_lookup(fp);
            if (se) {
                char info[160];
                if (se->v.type == VFS_DIR)
                    snprintf(info, sizeof(info), "%s  -  folder", se->path);
                else
                    snprintf(info, sizeof(info), "%s  -  %u bytes%s",
                             se->path, se->size, se->has_text ? "  (viewable)" : "");
                status_marquee(info, 160, H - 24, W - 160 - 16,
                               rgb(90, 90, 95));
            }
        }
        draw_drag(c, W);
        draw_msg(c);
        return;
    }

    /* main area responds to the selected favorite:
     * 0/1 = Applications (apps + the System HD drive icon)
     * 2   = home: the user's own files
     * 3   = Documents (empty) */
    int cols = grid_cols(), cell_w = grid_px(), cell_h = 110;
    int start_x = 160, start_y = 90;
    if (c->fav == 1 || c->fav == 0) {
        app_text(160, 50, "Applications", rgb(50, 50, 50), 2);
        int n = gui_app_count();
        for (int i = 0; i < n; i++) {
            App *a = gui_get_app(i);
            int col = i % cols, row = i / cols;
            int ix = start_x + col * cell_w;
            int iy = start_y + row * cell_h;
            if (ix + 64 > W || iy + 90 > H) break;
            if (c->sel == i) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
            if (c->prs == i) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
            } else if (c->hov == i && c->sel != i) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
            }
            asset_draw(ix, iy, 64, a->name);
            grid_label(ix + 32, iy, a->name);
        }
        /* System HD drive icon lives at the end of the Applications grid,
         * same 64px cell + centered label as every app icon; clicking it
         * opens the drive internals straight away */
        {
            int n2 = gui_app_count();
            int col = n2 % cols, row = n2 / cols;
            int ix = start_x + col * cell_w;
            int iy = start_y + row * cell_h;
            if (ix + 64 <= W && iy + 90 <= H) {
                if (c->sel == -4) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
                if (c->prs == -4) {
                    app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
                } else if (c->hov == -4 && c->sel != -4) {
                    app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
                }
                asset_draw(ix, iy, 64, "hd");
                grid_label(ix + 32, iy, "System HD");
            }
        }
        /* detected drives (USB / optical / VM-added) continue the grid
         * right after the System HD icon; ids -10-i in hit-tests */
        {
            int n2 = gui_app_count() + 1;   /* apps + System HD cell */
            int nd = ata_media_count();
            for (int di = 0; di < nd; di++) {
                int col = (n2 + di) % cols, row = (n2 + di) / cols;
                int ix = start_x + col * cell_w;
                int iy = start_y + row * cell_h;
                if (ix + 64 > W || iy + 90 > H) break;
                int id = -10 - di;
                if (c->sel == id) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
                if (c->prs == id) {
                    app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
                } else if (c->hov == id && c->sel != id) {
                    app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
                }
                asset_draw(ix, iy, 64, "hd");
                char lbl[48];
                AtaMediaInfo mi = { 0, 0, 0, "" };
                ata_media_info(di, &mi);
                if (mi.atapi) strcpy(lbl, "CD/DVD");
                else if (mi.removable) strcpy(lbl, "USB Drive");
                else strcpy(lbl, "Disk");
                grid_label(ix + 32, iy, lbl);
            }
        }
    } else if (c->fav == 2) {
        /* home: the user's own files, dynamic grid that fills the window
         * and scrolls (wheel + draggable bar) when it overflows */
        app_text(160, 14, gui_user_name(), rgb(50, 50, 50), 2);
        int vc = root_user_count();
        int gy = 50;
        cell_h = grid_py(grid_rows(vc, cols));
        int ty = 0, th = 0, thumb_y = 0, thumb_h = 0, maxsc = 0;
        fbar_geom(c, vc, &ty, &th, &thumb_y, &thumb_h, &maxsc);
        if (c->scroll > maxsc) c->scroll = maxsc;
        if (c->scroll < 0) c->scroll = 0;
        for (int j = 0; j < vc; j++) {
            const VfsEntry *e = root_user(j);
            if (!e) break;
            int col = j % cols, row = j / cols;
            int ix = start_x + col * cell_w;
            int iy = gy + row * cell_h - c->scroll;
            if (ix + 64 > W) break;
            if (iy < 38 || iy + 90 > H - 30) continue;
            int id = 100 + j;
            if (c->sel == id) app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 60));
            if (c->prs == id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(104, 0, 180, 120));
            } else if (c->hov == id && c->sel != id) {
                app_fill(ix - 4, iy - 4, 72, 72, rgba(138, 0, 255, 45));
            }
            if (!draw_item_icon(c, ix, iy, 64, e, j))
                asset_draw(ix, iy, 64, e->type == VFS_DIR ? "files" : gui_file_icon(e->name));
            grid_label(ix + 32, iy, e->name);
        }
        if (maxsc > 0) {
            app_fill(W - 12, ty, 6, th, rgb(210, 210, 210));
            app_fill(W - 12, thumb_y, 6, thumb_h, rgb(150, 150, 155));
        }
        if (vc == 0)
            app_text(160, 120, "This folder is empty.", rgb(110, 110, 115), 2);
    } else {
        /* Documents */
        app_text(160, 60, "This folder is empty.", rgb(110, 110, 115), 2);
    }
    draw_drag(c, W);
    draw_msg(c);
}

static int find_hit(FinderCtx *c, int x, int y);

/* drag ghost + drop-target highlight (content coords) */
/* dynamic grid: as many 110px columns as fit between sidebar and the
 * scrollbar, so folders fill the whole window instead of leaving a
 * fixed 4-column strip with empty space on the right */
static int grid_cols(void) {
    int c = (app_content_w() - 160 - 26) / 110;
    return c < 1 ? 1 : c;
}
static int grid_rows(int cells, int cols) {
    return (cells + cols - 1) / cols;
}
/* horizontal cell pitch: stretch columns across the whole content width
 * so there is no dead strip on the right (min 110px design pitch) */
static int grid_px(void) {
    int cols = grid_cols();
    int p = (app_content_w() - 160 - 30) / cols;
    return p < 110 ? 110 : p;
}
/* vertical row pitch: when the grid fits without scrolling, spread the
 * rows across the content height (capped so single rows don't float);
 * when it overflows keep the compact 110px pitch + scrollbar */
static int grid_py(int rows) {
    if (rows < 1) rows = 1;
    int avail = app_content_h() - 50 - 34;
    if (rows * 110 <= avail) return avail / rows;   /* >=110, fills height */
    return 110;
}
/* browse scrollbar geometry (content coords): track, thumb, max scroll */
static void sbar_geom(FinderCtx *c, int vc, int *ty, int *th,
                      int *thumb_y, int *thumb_h, int *maxsc) {
    int cols = grid_cols();
    *ty = 50;
    *th = app_content_h() - 50 - 34;
    int rows = grid_rows(vc + 1, cols);
    int content_h = rows * grid_py(rows);
    *maxsc = content_h - *th;
    if (*maxsc < 0) *maxsc = 0;
    int t = content_h > 0 ? (*th) * (*th) / content_h : *th;
    if (t < 20) t = 20;
    if (t > *th) t = *th;
    *thumb_h = t;
    int sc = c->scroll; if (sc < 0) sc = 0;
    if (sc > *maxsc) sc = *maxsc;
    *thumb_y = *ty + (*th - t) * sc / (*maxsc ? *maxsc : 1);
}

/* home-view (Files) scrollbar geometry: same track as browse but the
 * grid has no ".." row, so the cell count is just vc */
static void fbar_geom(FinderCtx *c, int vc, int *ty, int *th,
                      int *thumb_y, int *thumb_h, int *maxsc) {
    int cols = grid_cols();
    *ty = 50;
    *th = app_content_h() - 50 - 34;
    int rows = grid_rows(vc, cols);
    int content_h = rows * grid_py(rows);
    *maxsc = content_h - *th;
    if (*maxsc < 0) *maxsc = 0;
    int t = content_h > 0 ? (*th) * (*th) / content_h : *th;
    if (t < 20) t = 20;
    if (t > *th) t = *th;
    *thumb_h = t;
    int sc = c->scroll; if (sc < 0) sc = 0;
    if (sc > *maxsc) sc = *maxsc;
    *thumb_y = *ty + (*th - t) * sc / (*maxsc ? *maxsc : 1);
}

static void draw_drag(FinderCtx *c, int W) {
    /* in-app context menu (Rename/Delete for items, Eject/Refresh/
     * Properties for disks), drawn above everything */
    if (c->menu_open) {
        int mh2 = app_content_h();
        int disk = (c->menu_dev >= 0);
        static const char *mi_d[] = { "Eject", "Refresh", "Properties" };
        static const char *mi_f[] = { "Rename", "Delete" };
        const char **mi = disk ? mi_d : mi_f;
        int nitems = disk ? 3 : 2;
        int mw = 130, mh = 12 + nitems * 24;
        int mx = c->menu_x, my = c->menu_y;
        if (mx + mw > W) mx = W - mw - 4;
        if (my + mh > mh2) my = mh2 - mh - 4;
        app_blend(mx + 3, my + 3, mw, mh, rgba(0, 0, 0, 90));
        app_rounded(mx, my, mw, mh, 6, rgba(44, 13, 48, 235));
        app_rect(mx, my, mw, mh, rgb(182, 96, 200));
        for (int i = 0; i < nitems; i++) {
            int ry = my + 6 + i * 24;
            bool hov = (c->mx >= mx + 4 && c->mx < mx + mw - 4 &&
                        c->my >= ry && c->my < ry + 20);
            if (hov) app_fill(mx + 4, ry, mw - 8, 20, rgba(150, 70, 170, 150));
            app_text(mx + 12, ry + 5, mi[i], rgb(249, 232, 255), 1);
        }
    }
    if (!c->dragging || c->drag_id < 0) return;
    int t = find_hit(c, c->mx, c->my);
    int hx = -1, hy = -1;
    bool brows = (c->folder[0] || c->drive);
    if (t == 199 && brows) { hx = 156; hy = 46; }
    else if (t >= 100) {
        const VfsEntry *e = browse_entry(c, t - 100);
        if (e && e->type == VFS_DIR && !e->system) {
            int i = t - 100, col, row, iy;
            if (brows) {
                int gc = grid_cols(), px = grid_px();
                int py = grid_py(grid_rows(browse_count(c) + 1, gc));
                col = (i + 1) % gc; row = (i + 1) / gc;
                hx = 160 + col * px - 4; hy = 50 + row * py - c->scroll - 4;
            } else {
                /* home Files grid (fav 2): scrollable, no ".." row */
                int gc = grid_cols(), px = grid_px();
                int py = grid_py(grid_rows(root_user_count(), gc));
                col = i % gc; row = i / gc;
                hx = 160 + col * px - 4;
                hy = 50 + row * py - c->scroll - 4;
            }
        }
    }
    if (hx >= 0) app_fill(hx, hy, 72, 72, rgba(138, 0, 255, 70));
    int gx = c->mx - c->drag_ox, gy = c->my - c->drag_oy;
    asset_draw(gx, gy, 64, c->drag_type == VFS_DIR ? "files" : gui_file_icon(c->drag_name));
    int lw = app_text_width(c->drag_name, 1);
    app_text(gx + (64 - lw) / 2, gy + 66, c->drag_name, rgb(50, 50, 50), 1);
}

/* transient denied banner, drawn above everything in the content area */
static void draw_msg(FinderCtx *c) {
    if (!c->msg[0]) return;
    if (gui_ticks() - c->msg_t > 2500) { c->msg[0] = 0; return; }
    int W = app_content_w();
    int mw = app_text_width(c->msg, 1) + 28;
    int mx = (W - mw) / 2, my = app_content_h() - 46;
    app_blend(mx + 3, my + 3, mw, 30, rgba(0, 0, 0, 90));
    app_rounded(mx, my, mw, 30, 6, rgba(44, 13, 48, 235));
    app_rect(mx, my, mw, 30, rgb(182, 96, 200));
    app_text(mx + 14, my + 10, c->msg, rgb(255, 210, 210), 1);
}

/* hit-test helper: returns icon index, -2 = HD, -1 = none (content coords) */
static int find_hit(FinderCtx *c, int x, int y) {
    if (c->dev >= 0) {
        /* foreign-volume browser: ".." + directory grid (same math as
         * browse mode so hover/press geometry matches exactly) */
        if (!vfsx_probe(c->dev)) return -1;
        int n = vfsx_count(c->dev, c->devdir);
        int cols = grid_cols(), px = grid_px();
        int rows = grid_rows(n + 1, cols), py = grid_py(rows);
        int start_x = 160, start_y = 50;
        int iy0 = start_y - c->scroll;
        if (x >= start_x - 4 && x < start_x + 68 && iy0 >= 38 &&
            y >= iy0 - 4 && y < iy0 + 90) return 199;
        for (int i = 0; i < n; i++) {
            int col = (i + 1) % cols, row = (i + 1) / cols;
            int ix = start_x + col * px, iy = start_y + row * py - c->scroll;
            if (iy < 38 || iy + 90 > app_content_h() - 30) continue;
            if (x >= ix - 4 && x < ix + 68 && y >= iy - 4 && y < iy + 90)
                return 100 + i;
        }
        return -1;
    }
    /* browse mode: ".." back entry + children grid (folder or drive root) */
    if (c->folder[0] || c->drive) {
        int start_x = 160, start_y = 50;
        int iy0 = start_y - c->scroll;   /* ".." scrolls with the grid */
        if (x >= start_x - 4 && x < start_x + 68 && iy0 >= 38 &&
            y >= iy0 - 4 && y < iy0 + 90) return 199;
        int vc = browse_count(c);
        int ty = 0, th = 0, ty2 = 0, th2 = 0, maxsc = 0;
        sbar_geom(c, vc, &ty, &th, &ty2, &th2, &maxsc);
        int cols = grid_cols(), px = grid_px();
        int py = grid_py(grid_rows(vc + 1, cols));
        for (int i = 0; i < vc; i++) {
            int col = (i + 1) % cols, row = (i + 1) / cols;
            int ix = start_x + col * px, iy = start_y + row * py - c->scroll;
            if (iy < 38 || iy + 90 > app_content_h() - 30) continue;
            if (x >= ix - 4 && x < ix + 68 && y >= iy - 4 && y < iy + 90)
                return 100 + i;
        }
        return -1;
    }
    /* Applications view (fav 0/1): app icons + System HD cell (-4) */
    if (c->fav == 1 || c->fav == 0) {
        int n = gui_app_count(), gc = grid_cols(), px = grid_px();
        for (int i = 0; i < n; i++) {
            int col = i % gc, row = i / gc;
            int ix = 160 + col * px, iy = 90 + row * 110;
            if (x >= ix - 4 && x < ix + 68 && y >= iy - 4 && y < iy + 104)
                return i;
        }
        int col = n % gc, row = n / gc;
        int hx = 160 + col * px, hy = 90 + row * 110;
        if (x >= hx - 4 && x < hx + 68 && y >= hy - 4 && y < hy + 104)
            return -4;   /* System HD drive icon */
        /* detected drives continue the grid after System HD */
        int nd = ata_media_count();
        for (int di = 0; di < nd; di++) {
            int k = n + 1 + di;
            int dc = k % gc, dr = k / gc;
            int dx = 160 + dc * px, dy = 90 + dr * 110;
            if (x >= dx - 4 && x < dx + 68 && y >= dy - 4 && y < dy + 104)
                return -10 - di;
        }
        return -1;
    }
    /* home Files view (fav 2): scrollable user-files grid */
    if (c->fav == 2) {
        int vc = root_user_count();
        int px = grid_px(), py = grid_py(grid_rows(vc, grid_cols()));
        for (int j = 0; j < vc; j++) {
            int col = j % grid_cols(), row = j / grid_cols();
            int ix = 160 + col * px, iy = 50 + row * py - c->scroll;
            if (iy < 38 || iy + 90 > app_content_h() - 30) continue;
            if (x >= ix - 4 && x < ix + 68 && y >= iy - 4 && y < iy + 90)
                return 100 + j;
        }
    }
    return -1;
}

/* global screen center of a grid hit - target point for the drop "plop" */
static void hit_center(FinderCtx *c, int t, int *gx, int *gy) {
    int cx = 0, cy = 0;
    if (c->folder[0] || c->drive) {
        if (t == 199) { cx = 160 + 34; cy = 50 - c->scroll + 40; }
        else {
            int gc = grid_cols(), px = grid_px();
            int py = grid_py(grid_rows(browse_count(c) + 1, gc));
            int i = t - 100, col = (i + 1) % gc, row = (i + 1) / gc;
            cx = 160 + col * px + 34;
            cy = 50 + row * py - c->scroll + 45;
        }
    } else if (t == -4) {
        /* System HD icon in the Applications grid (64px cell) */
        int n = gui_app_count(), gc = grid_cols(), px = grid_px();
        cx = 160 + (n % gc) * px + 34;
        cy = 90 + (n / gc) * 110 + 45;
    } else if (t >= 100) {
        /* home Files grid (fav 2) */
        int j = t - 100, gc = grid_cols(), px = grid_px();
        int py = grid_py(grid_rows(root_user_count(), gc));
        cx = 160 + (j % gc) * px + 34;
        cy = 50 + (j / gc) * py - c->scroll + 45;
    } else if (t >= 0) {
        int gc = grid_cols(), px = grid_px();
        cx = 160 + (t % gc) * px + 34;
        cy = 90 + (t / gc) * 110 + 45;
    }
    *gx = app_content_x() + cx;
    *gy = app_content_y() + cy;
}

/* asset name for the flying plop icon of the dragged item */
static const char *drag_icon(FinderCtx *c) {
    if (c->drag_app) return c->drag_name;
    if (c->drag_type == VFS_DIR) return "files";
    return gui_file_icon(c->drag_name);
}

/* sidebar favorite rows: y = 30 + i*18, x < 140 (content coords);
 * Disk rows sit under the "Disk" header at y = 134 + i*18 */
static int fav_hit(int x, int y) {
    if (x < 4 || x >= 140 || y < 27 || y >= 27 + 4 * 18) return -1;
    return (y - 27) / 18;
}
static int dev_hit(int x, int y) {
    if (x < 4 || x >= 140 || y < 131) return -1;
    int i = (y - 134) / 18;
    if (i < 0 || i >= ata_media_count() || i >= 5) return -1;
    return i;
}

/* copy a file OUT of a foreign volume into the user's folder */
static void dev_copy_out(FinderCtx *c, int slot, const char *name) {
    int sz = vfsx_size(c->dev, slot);
    if (sz <= 0) { msg_set(c, "This file is empty or unreadable."); return; }
    if (sz > 1024 * 1024) { msg_set(c, "File is too large to copy."); return; }
    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) { msg_set(c, "Not enough memory to copy."); return; }
    int got = vfsx_read(c->dev, slot, buf, sz);
    if (got <= 0) {
        kfree(buf); msg_set(c, "Couldn't read from this disk."); return;
    }
    const char *uf = gui_user_name();
    const char *dir = (vfs_find(uf) >= 0) ? uf : NULL;
    char cand[VFS_NAME_LEN + 1];
    for (int k = 1; k < 100; k++) {
        if (k == 1) {
            strncpy(cand, name, sizeof(cand) - 1); cand[sizeof(cand)-1] = 0;
        } else {
            char base[VFS_NAME_LEN + 1];
            strncpy(base, name, sizeof(base) - 1); base[sizeof(base)-1] = 0;
            snprintf(cand, sizeof(cand), "%s-%d", base, k);
        }
        if (vfs_find_in(dir, cand) < 0) break;
        if (k == 99) { kfree(buf); msg_set(c, "Too many copies here."); return; }
    }
    if (vfs_create_in(cand, VFS_FILE, dir) != 0 ||
        vfs_write_in(dir, cand, (const char *)buf, got) != 0) {
        kfree(buf); msg_set(c, "Copy failed - disk may be full."); return;
    }
    kfree(buf);
    char m[64];
    snprintf(m, sizeof(m), "Copied %s to %s", cand, dir ? dir : "desktop");
    msg_set(c, m);
}

/* full untruncated path of sysvol entry #idx inside c->sysdir.
 * e->name is capped at VFS_NAME_LEN so long filenames (the bundled
 * music tracks!) lose their extension - sysvol_ent keeps the real
 * path, which is what lookups and the player actually need */
static void drive_entry_path(FinderCtx *c, int idx, char *buf, size_t cap) {
    const SysVolEnt *se = sysvol_ent(c->sysdir, idx);
    if (se && se->path[0]) {
        strncpy(buf, se->path, cap - 1); buf[cap - 1] = 0; return;
    }
    const VfsEntry *e = sysvol_entry(c->sysdir, idx);
    snprintf(buf, cap, "%s%s%s", c->sysdir, c->sysdir[0] ? "/" : "",
             e ? e->name : "");
}

/* double-click target: open/drill/navigate the pressed item. Single click
 * only selects - this is what lets press+drag move items without opening. */
static void open_hit(FinderCtx *c, int hit) {
    if (hit <= -10) {
        /* detected drive icon -> its volume browser */
        c->dev = -10 - hit;
        c->folder[0] = 0; c->drive = 0; c->sysdir[0] = 0;
        c->devdir = -1;
        c->sel = -1; c->scroll = 0;
        side_note_change(c);
        return;
    }
    if (c->dev >= 0) {
        /* inside a foreign volume: drill folders, copy files out */
        if (hit == 199) {
            if (c->devdir >= 0) {
                int p = vfsx_parent(c->dev, c->devdir);
                c->devdir = (p < 0) ? -1 : p;
            }
            c->sel = -1; c->scroll = 0;
            return;
        }
        if (hit >= 100) {
            VfsEntry e; int slot = -1;
            if (vfsx_get(c->dev, c->devdir, hit - 100, &e, &slot)) {
                if (e.type == VFS_DIR) {
                    c->devdir = slot; c->sel = -1; c->scroll = 0;
                } else dev_copy_out(c, slot, e.name);
            }
        }
        return;
    }
    if (hit == -4) {
        /* System HD icon in Applications -> straight into the drive;
         * move the sidebar highlight onto the System HD favorite too */
        c->drive = 1; c->folder[0] = 0; c->sysdir[0] = 0;
        find_set_fav(c, 0);
        c->dev = -1;
        side_note_change(c);
        c->sel = -1; c->scroll = 0;
        return;
    }
    if (hit >= 100 && (c->folder[0] || c->drive)) {
        /* browse contents: folders drill in; files open in Quill
         * (system-volume files open read-only from the ISO modules) */
        const VfsEntry *e = browse_entry(c, hit - 100);
        if (e && e->type == VFS_DIR) {
            if (c->drive) {
                /* store the entry's REAL path so deep browsing and
                 * file lookup survive long folder names */
                drive_entry_path(c, hit - 100, c->sysdir, sizeof(c->sysdir));
            } else {
                strncpy(c->folder, e->name, sizeof(c->folder) - 1);
                c->folder[sizeof(c->folder) - 1] = 0;
            }
            c->sel = -1; c->scroll = 0;
        } else if (e && c->drive) {
            char fp[160];
            drive_entry_path(c, hit - 100, fp, sizeof(fp));
            const SysVolEnt *se = sysvol_lookup(fp);
            const char *tp; uint32_t tn;
            int nl = (int)strlen(e->name);
            bool ispng = gui_is_png(e->name);
            if (ispng && se && se->has_blob) {
                /* real ISO image -> Photos viewer, read-only */
                gui_set_pending_sysimg(fp);
                gui_open_app_new(app_make_photos());
            } else if (e && gui_is_audio(e->name)) {
                /* .mp3/.wav/.ogg on System HD -> Voyage Player plays it */
                gui_set_pending_audio(fp);
                App *a = gui_find_app("Voyage Player");
                if (a) gui_open_app_new(a);
                c->sel = hit;
            } else if (se && sysvol_text(se, &tp, &tn) == 0) {
                gui_set_pending_sysfile(fp);
                App *q = gui_find_app("Quill");
                if (q) gui_open_app_new(q);
            } else {
                char m[80];
                snprintf(m, sizeof(m), "%s - binary system file",
                         e->name);
                msg_set(c, m);
            }
            c->sel = hit;
        } else if (e && !c->drive && open_lnk(c->folder, e->name)) {
            c->sel = hit;   /* ".lnk" shortcut -> launched the app */
        } else if (e) {
            /* .txt -> Quill, .png -> Photos, rest -> invalid */
            int nl = (int)strlen(e->name);
            bool istxt = nl > 4 && !strcmp(e->name + nl - 4, ".txt");
            bool ispng = gui_is_png(e->name);
            if (istxt) {
                gui_set_pending_path(c->folder, e->name);
                App *q = gui_find_app("Quill");
                if (q) gui_open_app_new(q);
            } else if (ispng) {
                gui_set_pending_image(c->folder, e->name);
                gui_open_app_new(app_make_photos());
            } else if (gui_is_audio(e->name)) {
                char ap[160];
                snprintf(ap, sizeof(ap), "%s%s%s", c->folder,
                         c->folder[0] ? "/" : "", e->name);
                gui_set_pending_audio(ap);
                App *a = gui_find_app("Voyage Player");
                if (a) gui_open_app_new(a);
            } else {
                msg_set(c, "This file is invalid or corrupted!");
            }
            c->sel = hit;
        }
        return;
    }
    if (hit >= 100) {
        /* VFS item: ".lnk" launches its app, .txt opens in Quill */
        const VfsEntry *e = root_user(hit - 100);
        if (e && e->type == VFS_FILE) {
            if (!open_lnk(NULL, e->name)) {
                int nl = (int)strlen(e->name);
                bool istxt = nl > 4 && !strcmp(e->name + nl - 4, ".txt");
                bool ispng = gui_is_png(e->name);
                if (istxt) {
                    gui_set_pending_file(e->name);
                    App *q = gui_find_app("Quill");
                    if (q) gui_open_app_new(q);
                } else if (ispng) {
                    gui_set_pending_image("", e->name);
                    gui_open_app_new(app_make_photos());
                } else if (gui_is_audio(e->name)) {
                    gui_set_pending_audio(e->name);
                    App *a = gui_find_app("Voyage Player");
                    if (a) gui_open_app_new(a);
                } else {
                    msg_set(c, "This file is invalid or corrupted!");
                }
            }
        }
        c->sel = hit;
        return;
    }
    if (hit >= 0) {
        App *a = gui_get_app(hit);
        if (a) gui_open_app_new(a);
    }
}

static void find_event(void *ctx, GuiEvent *ev) {
    FinderCtx *c = (FinderCtx *)ctx;
    if (ev->type == EV_MOUSE_MOVE) {
        c->mx = ev->x; c->my = ev->y;
        /* scrollbar thumb drag takes priority over icon hover/drag */
        if (c->sbar >= 0 &&
            (c->folder[0] || c->drive || c->fav == 2)) {
            int ty = 0, th = 0, thumb_y = 0, thumb_h = 0, maxsc = 0;
            if (c->folder[0] || c->drive)
                sbar_geom(c, browse_count(c), &ty, &th, &thumb_y, &thumb_h, &maxsc);
            else
                fbar_geom(c, root_user_count(), &ty, &th, &thumb_y, &thumb_h, &maxsc);
            int travel = th - thumb_h;
            if (travel > 0 && maxsc > 0) {
                int ns = (ev->y - ty - c->sbar) * maxsc / travel;
                if (ns < 0) ns = 0;
                if (ns > maxsc) ns = maxsc;
                c->scroll = ns;
            }
            return;
        }
        if (c->dragging) return;   /* ghost follows via stored pos */
        /* pressed on an icon (VFS item or built-in app) and moved -> drag */
        if (c->prs >= 0 && c->prs != 199 && !c->dragging) {
            int dx = ev->x - c->prs_x, dy = ev->y - c->prs_y;
            if (dx * dx + dy * dy >= 36) {
                c->dragging = 1;
                c->drag_id = c->prs;
                c->drag_app = 0;
                c->drag_fx = -1;
                int ix = 0, iy = 0;
                if (c->dev >= 0 && c->prs >= 100) {
                    /* foreign volume file: drag it OUT - dropping copies
                     * it into the user's folder. Folders can't drag
                     * (no recursive copy) so they abort like apps do. */
                    VfsEntry fe; int fslot = -1;
                    if (vfsx_get(c->dev, c->devdir, c->prs - 100, &fe, &fslot) &&
                        fe.type == VFS_FILE) {
                        c->drag_type = VFS_FILE; c->drag_sys = 0;
                        c->drag_fx = fslot;
                        strncpy(c->drag_name, fe.name,
                                sizeof(c->drag_name) - 1);
                        c->drag_name[sizeof(c->drag_name) - 1] = 0;
                        c->drag_src[0] = 0;
                        int i = c->prs - 100;
                        int gc = grid_cols(), px = grid_px();
                        int n = vfsx_count(c->dev, c->devdir);
                        int rows = grid_rows(n + 1, gc), py = grid_py(rows);
                        int col = (i + 1) % gc, row = (i + 1) / gc;
                        ix = 160 + col * px; iy = 50 + row * py - c->scroll;
                    } else c->drag_id = -2;
                } else if (c->prs >= 100) {
                    const VfsEntry *e = browse_entry(c, c->prs - 100);
                    if (e) {
                        c->drag_type = e->type;
                        c->drag_sys = e->system;    /* protected items can be
                                                     * dragged but not dropped */
                        strncpy(c->drag_name, e->name, sizeof(c->drag_name) - 1);
                        c->drag_name[sizeof(c->drag_name) - 1] = 0;
                    }
                    strncpy(c->drag_src, c->drive ? "" : c->folder, sizeof(c->drag_src) - 1);
                    c->drag_src[sizeof(c->drag_src) - 1] = 0;
                    /* grab offset inside the 64px icon (browse grid layout) */
                    int i = c->prs - 100, col, row;
                    if (c->folder[0] || c->drive) {
                        int gc = grid_cols(), px = grid_px();
                        int py = grid_py(grid_rows(browse_count(c) + 1, gc));
                        col = (i + 1) % gc; row = (i + 1) / gc;
                        ix = 160 + col * px; iy = 50 + row * py - c->scroll;
                    } else {
                        /* home Files grid (fav 2) */
                        int gc = grid_cols(), px = grid_px();
                        int py = grid_py(grid_rows(root_user_count(), gc));
                        col = i % gc; row = i / gc;
                        ix = 160 + col * px; iy = 50 + row * py - c->scroll;
                    }
                } else {
                    /* built-in app icon (only in the normal Files view) */
                    App *a = (!c->folder[0] && !c->drive)
                             ? gui_get_app(c->prs) : NULL;
                    if (a) {
                        c->drag_app = 1;
                        c->drag_type = VFS_FILE;
                        c->drag_sys = 0;
                        strncpy(c->drag_name, a->name, sizeof(c->drag_name) - 1);
                        c->drag_name[sizeof(c->drag_name) - 1] = 0;
                        c->drag_src[0] = 0;
                        int gc = grid_cols(), px = grid_px();
                        int col = c->prs % gc, row = c->prs / gc;
                        ix = 160 + col * px; iy = 90 + row * 110;
                    } else c->drag_id = -2;   /* not draggable: abort */
                }
                c->drag_ox = ev->x - ix; c->drag_oy = ev->y - iy;
                c->sel = c->prs;
            }
        }
        int fh = fav_hit(ev->x, ev->y);
        int dh = dev_hit(ev->x, ev->y);
        int fb2 = (c->folder[0] && !c->drive)
                  ? fb_hit(ev->x, ev->y, app_content_w()) : -1;
        c->hov_side = fb2 == 300 ? 20 : fb2 == 301 ? 21 :
                      fh >= 0 ? fh : dh >= 0 ? 10 + dh : -1;
        c->hov = fh >= 0 ? -3 : find_hit(c, ev->x, ev->y);  /* -3 = sidebar row */
        gui_set_cursor((fh >= 0 || dh >= 0 || fb2 >= 0 ||
                        c->hov != -1) ? CUR_HAND : -1);
        return;
    }
    if (ev->type == EV_MOUSE_WHEEL &&
        (c->folder[0] || c->drive || c->fav == 2 || c->dev >= 0)) {
        c->scroll -= ev->wheel * 50;    /* wheel up = scroll toward top */
        if (c->dev >= 0) {
            int n = vfsx_probe(c->dev) ? vfsx_count(c->dev, c->devdir) : 0;
            int rows = grid_rows(n + 1, grid_cols());
            int maxsc = grid_py(rows) * rows - (app_content_h() - 96);
            if (maxsc < 0) maxsc = 0;
            if (c->scroll < 0) c->scroll = 0;
            if (c->scroll > maxsc) c->scroll = maxsc;
            return;
        }
        int ty = 0, th = 0, tyy = 0, thh = 0, maxsc = 0;
        if (c->folder[0] || c->drive)
            sbar_geom(c, browse_count(c), &ty, &th, &tyy, &thh, &maxsc);
        else
            fbar_geom(c, root_user_count(), &ty, &th, &tyy, &thh, &maxsc);
        if (c->scroll < 0) c->scroll = 0;
        if (c->scroll > maxsc) c->scroll = maxsc;
        return;
    }
    /* scrollbar: drag the thumb, or click the track to page */
    if ((c->folder[0] || c->drive || c->fav == 2) && c->sbar >= -1) {
        int ty = 0, th = 0, thumb_y = 0, thumb_h = 0, maxsc = 0;
        if (c->folder[0] || c->drive)
            sbar_geom(c, browse_count(c), &ty, &th, &thumb_y, &thumb_h, &maxsc);
        else
            fbar_geom(c, root_user_count(), &ty, &th, &thumb_y, &thumb_h, &maxsc);
        int W = app_content_w();
        if (ev->type == EV_MOUSE_DOWN && ev->button == 0 && maxsc > 0 &&
            ev->x >= W - 14 && ev->x < W - 4 &&
            ev->y >= ty && ev->y < ty + th) {
            if (ev->y >= thumb_y && ev->y < thumb_y + thumb_h)
                c->sbar = ev->y - thumb_y;          /* grab the thumb */
            else {
                /* page up/down by ~3/4 of the visible area */
                int page = th * 3 / 4;
                c->scroll += (ev->y < thumb_y) ? -page : page;
                if (c->scroll < 0) c->scroll = 0;
                if (c->scroll > maxsc) c->scroll = maxsc;
            }
            return;
        }
        if (ev->type == EV_MOUSE_UP) c->sbar = -1;
    }
    if (ev->type == EV_MOUSE_UP) {
        if (c->dragging) {
            /* drop: over ".." -> move to parent; over folder -> move in;
             * app icons dropped on a folder create a ".lnk" shortcut */
            int t = find_hit(c, ev->x, ev->y);
            bool attempted = false, failed = false, landed = false;
            int tx = 0, ty = 0;
            if (t >= 0) hit_center(c, t, &tx, &ty);
            /* foreign-volume file dragged out of a disk view:
             * drop on another disk = disk-to-disk copy, drop anywhere
             * else = copy into the user's folder, drop back on the
             * disk grid = cancel (it never left) */
            if (c->drag_fx >= 0) {
                int dh3 = dev_hit(ev->x, ev->y);
                int tgt = -1;
                if (dh3 >= 0) tgt = dh3;
                else if (t <= -10) tgt = -10 - t;
                if (tgt >= 0 && tgt != c->dev) {
                    attempted = true;
                    int sz = vfsx_size(c->dev, c->drag_fx);
                    uint8_t *buf = sz > 0 ? (uint8_t *)kmalloc(sz) : NULL;
                    int n = buf ? vfsx_read(c->dev, c->drag_fx, buf, sz) : -1;
                    if (n > 0 && vfsx_probe(tgt) &&
                        vfsx_write(tgt, -1, c->drag_name,
                                   (const char *)buf, n) == 0) {
                        landed = true;
                        msg_set(c, "Copied to disk");
                    } else failed = true;
                    if (buf) kfree(buf);
                } else if (t < 100) {
                    dev_copy_out(c, c->drag_fx, c->drag_name);
                }
                c->dragging = 0; c->drag_id = -1; c->prs = -1;
                c->prs_side = -1;
                c->drag_fx = -1;
                return;
            }
            /* drop a user file onto a detected disk (sidebar row or
             * Applications-grid drive icon) -> copy into its root */
            int midx = -1;
            int dh2 = dev_hit(ev->x, ev->y);
            if (dh2 >= 0) midx = dh2;
            else if (t <= -10) midx = -10 - t;
            if (midx >= 0 && !c->drag_app && c->drag_id >= 100 &&
                c->drag_type == VFS_FILE) {
                attempted = true;
                const char *src = c->drag_src[0] ? c->drag_src : NULL;
                int sz = vfs_file_size_in(src, c->drag_name);
                if (sz <= 0) sz = 512;
                uint8_t *buf = (uint8_t *)kmalloc(sz);
                int n = buf ? vfs_read_raw_full_in(src, c->drag_name,
                                                   buf, sz) : -1;
                if (n > 0 && vfsx_probe(midx) &&
                    vfsx_write(midx, -1, c->drag_name,
                               (const char *)buf, n) == 0) {
                    landed = true;
                    msg_set(c, "Copied to disk");
                } else failed = true;
                if (buf) kfree(buf);
                c->dragging = 0; c->drag_id = -1; c->prs = -1;
                c->prs_side = -1;
                c->drag_fx = -1;
                return;
            }
            if (c->drag_app) {
                if (t >= 100) {
                    const VfsEntry *e = browse_entry(c, t - 100);
                    if (e && e->type == VFS_DIR && !e->system) {
                        attempted = true;
                        make_lnk(e->name, c->drag_name);
                        landed = true;
                    }
                }
            } else if (t == 199 && !c->drive && c->folder[0]) {
                attempted = true;
                char par[VFS_NAME_LEN + 1];
                if (vfs_parent_name(c->folder, par, sizeof(par)) != 0 ||
                    vfs_move(c->drag_name, c->drag_src,
                             par[0] ? par : NULL) != 0)
                    failed = true;
                else landed = true;
            } else if (t >= 100 && c->drag_id >= 100 && t != c->drag_id) {
                const VfsEntry *e = browse_entry(c, t - 100);
                if (e && e->type == VFS_DIR) {
                    attempted = true;
                    if (e->system ||
                        vfs_move(c->drag_name, c->drag_src[0] ? c->drag_src : NULL,
                                 e->name) != 0)
                        failed = true;
                    else landed = true;
                }
            }
            if (landed)
                gui_plop(app_content_x() + ev->x, app_content_y() + ev->y,
                         tx, ty, drag_icon(c), c->drag_src, c->drag_name);
            /* dropped OUTSIDE the window -> onto the desktop (root):
             * files/folders move to root, app icons become ".lnk" there */
            bool outside = ev->x < -6 || ev->y < -6 ||
                           ev->x > app_content_w() + 6 ||
                           ev->y > app_content_h() + 6;
            if (outside && !c->drive && !c->drag_sys) {
                if (c->drag_app) {
                    gui_drop_app_shortcut(c->drag_name,
                        app_content_x() + ev->x, app_content_y() + ev->y);
                    int sx = 0, sy = 0;
                    hit_center(c, c->drag_id, &sx, &sy);
                    gui_plop_out(sx, sy, app_content_x() + ev->x,
                                 app_content_y() + ev->y,
                                 c->drag_name, NULL, NULL);
                } else if (c->drag_src[0]) {
                    if (vfs_move(c->drag_name, c->drag_src, NULL) == 0) {
                        /* pin the icon exactly where it was dropped, not
                         * at the next free grid cell */
                        gui_place_icon_at(c->drag_name,
                            app_content_x() + ev->x,
                            app_content_y() + ev->y);
                        /* reverse plop: slides OUT of the folder grid to
                         * the cursor, like being taken out */
                        int sx = 0, sy = 0;
                        hit_center(c, c->drag_id, &sx, &sy);
                        gui_plop_out(sx, sy, app_content_x() + ev->x,
                                     app_content_y() + ev->y,
                                     drag_icon(c), "", c->drag_name);
                    }
                }
            } else if (outside && c->drag_sys) deny(c);
            if (attempted && failed) deny(c);
            c->dragging = 0; c->drag_id = -1; c->prs = -1;
            c->prs_side = -1;
            c->drag_fx = -1;
            return;
        }
        /* no drag: double-click on the pressed item opens it */
        {
            int hit = find_hit(c, ev->x, ev->y);
            if (hit == c->prs && hit != -1) {
                uint64_t now = gui_ticks();
                if (now - c->last_ms < 500 && c->last_id == hit)
                    open_hit(c, hit);
                c->last_id = hit; c->last_ms = now;
            }
        }
        c->prs = -1; c->prs_side = -1; return;
    }
    /* context menu open? clicks choose an action, outside closes */
    if (c->menu_open && ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int W = app_content_w(), H = app_content_h();
        int disk = (c->menu_dev >= 0);
        int nitems = disk ? 3 : 2;
        int mw = 130, mh = 12 + nitems * 24;
        int mx = c->menu_x, my = c->menu_y;
        if (mx + mw > W) mx = W - mw - 4;
        if (my + mh > H) my = H - mh - 4;
        bool in = (ev->x >= mx && ev->x < mx + mw && ev->y >= my && ev->y < my + mh);
        c->menu_open = 0;
        if (in) {
            int row = (ev->y - my - 6) / 24;
            if (disk) {
                int md = c->menu_dev;
                if (row == 0) {           /* Eject */
                    AtaMediaInfo emi = { 0, 0, 0, "" };
                    ata_media_info(md, &emi);
                    if (!emi.removable && !emi.atapi) {
                        msg_set(c, "This disk can't be ejected.");
                    } else if (ata_eject_media(md) == 0) {
                        if (c->dev == md) { c->dev = -1; side_note_change(c); }
                        msg_set(c, "Disk ejected.");
                    } else msg_set(c, "Couldn't eject that disk.");
                } else if (row == 1) {    /* Refresh */
                    ata_media_refresh();
                    if (c->dev >= ata_media_count()) c->dev = -1;
                    msg_set(c, "Disks refreshed.");
                } else if (row == 2) {    /* Properties */
                    AtaMediaInfo mi = { 0, 0, 0, "" };
                    ata_media_info(md, &mi);
                    char pbuf[256];
                    const char *kind = mi.atapi ? "CD/DVD Drive" :
                                       mi.removable ? "USB Drive" : "Disk";
                    char cap[32] = "";
                    if (mi.sectors && !mi.atapi) {
                        unsigned mb = (unsigned)(mi.sectors / 2048);
                        if (mb >= 1024)
                            snprintf(cap, sizeof(cap), "%u.%u GB",
                                     mb / 1024, (mb % 1024) * 10 / 1024);
                        else
                            snprintf(cap, sizeof(cap), "%u MB", mb);
                    }
                    snprintf(pbuf, sizeof(pbuf),
                             "Name: %s\nKind: %s\nModel: %s\nCapacity: %s",
                             mi.model[0] ? mi.model : kind, kind,
                             mi.model[0] ? mi.model : "-",
                             cap[0] ? cap : "-");
                    gui_alert(2, "Disk Properties", pbuf);
                }
                c->menu_dev = -1;
            } else {
                const VfsEntry *e = NULL;
                if (c->menu_hit >= 100)
                    e = browse_entry(c, c->menu_hit - 100);
                if (e && e->system) {
                    deny(c);    /* protected: rename/delete -> denied */
                } else if (e && row == 0) {
                    gui_request_rename(c->drive || !c->folder[0] ? NULL : c->folder,
                                       e->name);
                } else if (e && row == 1) {
                    if (e->type == VFS_DIR && finder_folder_in_use(e->name))
                        msg_set(c, "Directory is still in use!");
                    else {
                        vfs_delete_in(c->drive || !c->folder[0] ? NULL : c->folder,
                                      e->name);
                        if (c->sel == c->menu_hit) c->sel = -1;
                    }
                }
            }
        }
        return;
    }
    /* right-click on a VFS item -> Rename/Delete menu; on a disk row,
     * drive icon, or inside a disk view -> Eject/Refresh/Properties */
    if (ev->type == EV_MOUSE_DOWN && ev->button == 1) {
        int hit = find_hit(c, ev->x, ev->y);
        int dd = dev_hit(ev->x, ev->y);
        if (dd < 0 && hit <= -10 && hit > -100) dd = -10 - hit;   /* drive icon */
        if (dd < 0 && c->dev >= 0) dd = c->dev;   /* disk view: disk menu */
        if (dd >= 0) {
            c->menu_open = 1; c->menu_dev = dd; c->menu_hit = -1;
            c->menu_x = ev->x; c->menu_y = ev->y;
        } else if (hit >= 100) {
            c->menu_open = 1; c->menu_hit = hit; c->menu_dev = -1;
            c->menu_x = ev->x; c->menu_y = ev->y;
        } else c->menu_open = 0;
        return;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        c->menu_open = 0;
        c->prs_x = ev->x; c->prs_y = ev->y;
        /* press feedback on every sidebar row + toolbar button */
        {
            int fh0 = fav_hit(ev->x, ev->y);
            int dh0 = dev_hit(ev->x, ev->y);
            int fb0 = (c->folder[0] && !c->drive)
                      ? fb_hit(ev->x, ev->y, app_content_w()) : -1;
            c->prs_side = fb0 == 300 ? 20 : fb0 == 301 ? 21 :
                          fh0 >= 0 ? fh0 : dh0 >= 0 ? 10 + dh0 : -1;
        }
        /* browse view: New Folder / New File buttons (user folders only) */
        if (c->folder[0]) {
            int fb = fb_hit(ev->x, ev->y, app_content_w());
            if (fb == 300 || fb == 301) {
                if (browse_system(c)) deny(c);
                else fb_create(c, fb == 300 ? 0 : 1);
                return;
            }
        }
        int dh = dev_hit(ev->x, ev->y);
        if (dh >= 0) {
            /* Disk sidebar row -> that drive's volume browser; the
             * unified pill GLIDES down from the favorite it left */
            c->dev = dh; c->fav = -1; c->sel = -1; c->devdir = -1;
            c->folder[0] = 0; c->drive = 0; c->sysdir[0] = 0; c->scroll = 0;
            side_note_change(c);
            return;
        }
        int fh = fav_hit(ev->x, ev->y);
        if (fh >= 0) {
            find_set_fav(c, fh); c->sel = -1; c->folder[0] = 0; c->scroll = 0;
            c->dev = -1;
            side_note_change(c);   /* pill glides back up into Favorites */
            if (fh == 0) {
                /* System HD favorite -> straight into the drive internals */
                c->drive = 1; c->sysdir[0] = 0;
            } else c->drive = 0;
            return;
        }
        int hit = find_hit(c, ev->x, ev->y);
        c->prs = hit;
        if (hit != -1) c->prs_t = gui_ticks();
        if (hit == 199) {
            /* ".." back: volume root -> home view; dir -> parent path */
            if (c->drive) {
                char *s = strrchr(c->sysdir, '/');
                if (s) *s = 0; else { c->drive = 0; c->sysdir[0] = 0; }
                c->sel = -1; c->scroll = 0;
                return;
            }
            int fi = vfs_find(c->folder);
            const VfsEntry *fe = (fi >= 0) ? vfs_entry(fi) : NULL;
            if (fe && fe->parent >= 0) {
                const VfsEntry *pe = vfs_entry(fe->parent);
                if (pe) strncpy(c->folder, pe->name, sizeof(c->folder) - 1);
                else c->folder[0] = 0;
            } else c->folder[0] = 0;
            c->folder[sizeof(c->folder) - 1] = 0;
            c->sel = -1; c->scroll = 0;
            return;
        }
        if (hit == -2) { c->sel = -2; c->drive = 1; c->folder[0] = 0; c->sysdir[0] = 0; c->dev = -1; find_set_fav(c, 0); side_note_change(c); c->scroll = 0; return; }
        /* single click only SELECTS - open/drill happens on double-click
         * release, so press+drag can move items without opening them */
        c->sel = hit;
    }
}
static void find_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Files", "Files", 700, 480, -1,
    find_create, find_destroy, find_init, find_draw, find_event, find_update, 0, 0
};
App *make_finder(void) { return &s_app; }
