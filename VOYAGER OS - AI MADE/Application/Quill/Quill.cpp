/* Quill.app - simple plain-text editor */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../gui/gui.h"
#include "../../kernel/vfs.h"
#include "../../kernel/sysvol.h"
#include "../../kernel/k.h"

#define TE_MAX 8192
#define TE_VIEW_W 100

typedef struct {
    char text[TE_MAX];
    int len;
    int cursor;      /* byte offset */
    int scroll;      /* first visible line */
    bool focused;
    bool editing;    /* caret shown only after clicking the text area */
    char file[VFS_NAME_LEN + 1];  /* VFS file backing this doc, "" = untitled */
    char dir[VFS_NAME_LEN + 1];   /* folder the file lives in, "" = root */
    bool dirty;
    uint64_t last_save;
    /* "select directory" picker for untitled docs */
    bool pick_active;
    int  pick_sel;
    char pick_name[48];  /* filename typed into the save picker */
    int  pick_cur;       /* cursor offset inside pick_name */
    bool pick_name_foc;  /* name box has keyboard focus (default: yes) */
    bool read_only;      /* opened a protected system file: view only */
    char msg[64];        /* transient status ("This action is denied!") */
    uint64_t msg_t;
    int  pick_hov, pick_prs;  /* picker hover/press id: row idx,
                                 1000=Save 1001=Cancel -2=name -1 none */
} TE_Ctx;

static void te_deny(TE_Ctx *c) {
    snprintf(c->msg, sizeof(c->msg), "This action is denied!");
    c->msg_t = gui_ticks();
    gui_alert_beep();
}
static void te_msg(TE_Ctx *c, const char *m) {
    snprintf(c->msg, sizeof(c->msg), "%s", m);
    c->msg_t = gui_ticks();
    gui_alert_beep();
}
/* is `name` inside `dir` a protected system entry? */
static int te_sys_file(const char *dir, const char *name) {
    int n = dir[0] ? vfs_count_in(dir) : vfs_count();
    for (int i = 0; i < n; i++) {
        const VfsEntry *e = dir[0] ? vfs_entry_in(dir, i) : vfs_entry(i);
        if (e && strcmp(e->name, name) == 0) return e->system;
    }
    return 0;
}

static void te_insert(TE_Ctx *c, char ch) {
    if (c->len >= TE_MAX - 1) return;
    if (c->cursor < c->len)
        memmove(c->text + c->cursor + 1, c->text + c->cursor, c->len - c->cursor);
    c->text[c->cursor++] = ch;
    c->len++;
    c->text[c->len] = 0;
    c->dirty = true;
}

static void te_newline(TE_Ctx *c) { te_insert(c, '\n'); }

static void te_backspace(TE_Ctx *c) {
    if (c->cursor <= 0) return;
    memmove(c->text + c->cursor - 1, c->text + c->cursor, c->len - c->cursor);
    c->cursor--;
    c->len--;
    c->text[c->len] = 0;
    c->dirty = true;
}

/* write the doc into `dir` ("" = root); creates the VFS file when untitled.
 * `want_name` (may be NULL) overrides the auto "Untitled.txt" naming. */
static int te_do_save(TE_Ctx *c, const char *dir, const char *want_name) {
    if (c->read_only) { te_deny(c); return -1; }
    if (!c->dirty) return 0;
    /* disk-space guard: refuse the save before touching the VDI when
     * there is no contiguous free run left (files span whole sectors) */
    {
        int need = (int)((c->len < VFS_CONTENT ? c->len : VFS_CONTENT) + 511) / 512;
        if (need < 1) need = 1;
        if (vfs_free_run_sectors() < need) {
            te_msg(c, "Not enough disk space to save!");
            return -1;
        }
    }
    if (!c->file[0]) {
        char name[48];
        name[0] = 0;
        if (want_name) {
            /* sanitize: strip path separators, clamp to VFS name len */
            int ni = 0;
            for (const char *p = want_name; *p && ni < VFS_NAME_LEN; p++)
                if (*p != '/' && *p != '\\') name[ni++] = *p;
            name[ni] = 0;
        }
        if (!name[0]) snprintf(name, sizeof(name), "Untitled.txt");
        int made = 0;
        if (vfs_find_in(dir, name) >= 0) {
            /* exists: overwrite it in place */
            made = (vfs_write_in(dir, name, c->text,
                       c->len < VFS_CONTENT ? c->len : VFS_CONTENT) == 0);
        } else {
            made = (vfs_create_in(name, VFS_FILE, dir[0] ? dir : NULL) == 0 &&
                    vfs_write_in(dir, name, c->text,
                        c->len < VFS_CONTENT ? c->len : VFS_CONTENT) == 0);
        }
        if (!made) { kprintf("te: create '%s' in '%s' failed\n", name, dir); return -1; }
        strncpy(c->file, name, sizeof(c->file) - 1);
        c->file[sizeof(c->file) - 1] = 0;
        strncpy(c->dir, dir, sizeof(c->dir) - 1);
        c->dir[sizeof(c->dir) - 1] = 0;
    }
    int r = vfs_write_in(c->dir, c->file, c->text,
                         c->len < VFS_CONTENT ? c->len : VFS_CONTENT);
    if (r != 0) kprintf("te: write '%s' in '%s' failed r=%d\n", c->file, c->dir, r);
    if (r == 0) { c->dirty = false; c->last_save = gui_ticks(); }
    return r;
}

static void te_save(TE_Ctx *c) { te_do_save(c, c->dir, NULL); }

/* confirm_close hook: veto close while unsaved changes exist */
static int te_confirm(void *ctx) {
    TE_Ctx *c = (TE_Ctx *)ctx;
    return c->dirty ? 1 : 0;
}
/* save_now hook: named docs save in place; untitled opens dir picker */
static void te_save_now(void *ctx) {
    TE_Ctx *c = (TE_Ctx *)ctx;
    if (c->read_only) { te_deny(c); return; }
    if (!c->file[0]) {
        c->pick_active = true; c->pick_sel = 0;
        c->pick_name_foc = true;
        if (!c->pick_name[0]) {
            strcpy(c->pick_name, "Untitled.txt");
            c->pick_cur = (int)strlen(c->pick_name);
        }
    } else te_save(c);
}

static void *te_create(void) {
    TE_Ctx *c = (TE_Ctx *)kzalloc(sizeof(TE_Ctx));
    /* opened a file straight from the System HD (ISO) volume? view-only */
    const char *sp = gui_pending_sysfile();
    if (sp) {
        const SysVolEnt *se = sysvol_lookup(sp);
        const char *tp; uint32_t tn;
        if (se && sysvol_text(se, &tp, &tn) == 0) {
            uint32_t n = tn < (uint32_t)TE_MAX - 1 ? tn : (uint32_t)TE_MAX - 1;
            memcpy(c->text, tp, n);
            c->text[n] = 0;
            c->len = (int)n;
        }
        c->read_only = 1;
        gui_clear_pending_sysfile();
    }
    /* opened from a desktop file icon? load its contents */
    const char *pf = gui_pending_file();
    if (pf) {
        const char *pd = gui_pending_dir();
        if (pd) {
            strncpy(c->dir, pd, sizeof(c->dir) - 1);
            c->dir[sizeof(c->dir) - 1] = 0;
        }
        /* verify the file exists in that folder */
        if (vfs_find_in(c->dir, pf) >= 0) {
            strncpy(c->file, pf, sizeof(c->file) - 1);
            c->file[sizeof(c->file) - 1] = 0;
            char buf[VFS_CONTENT + 1];
            int n = vfs_read_in(c->dir, pf, buf, sizeof(buf) - 1);
            if (n > 0) {
                if (n > TE_MAX - 1) n = TE_MAX - 1;
                memcpy(c->text, buf, n);
                c->text[n] = 0;
                c->len = n;
            }
            c->read_only = te_sys_file(c->dir, pf) != 0;
            gui_clear_pending_file();
        }
    } else if (!sp) {
        strcpy(c->text, "Welcome to Quill!\n\nType here...\n");
        c->len = strlen(c->text);
    }
    c->cursor = c->len;
    c->pick_hov = c->pick_prs = -1;
    return c;
}
static void te_init(void *ctx) { (void)ctx; }
static void te_destroy(void *ctx) {
    /* no save here: the "Do you want to save?" dialog + autosave cover it;
     * saving on destroy would defeat "Don't Save" */
    kfree(ctx);
}

#define TE_PAD 16
#define TE_LH 10   /* line height scale-1 font = 9, +1 spacing */

/* ---- visual-line wrap (Notepad-style soft wrap) ----
 * chars per visual line from usable pixel width / 9px glyph */
static int te_vchars(int W) {
    int n = (W - 2 * TE_PAD - 12) / 9;
    return n > 4 ? n : 4;
}
/* byte offset -> visual line & column (col may equal vcw = line end) */
static void te_pos_vis(TE_Ctx *c, int pos, int W, int *vl, int *vc) {
    int vcw = te_vchars(W), vline = 0, col = 0;
    for (int i = 0; i < pos && i < c->len; i++) {
        if (c->text[i] == '\n') { vline++; col = 0; continue; }
        if (col >= vcw) { vline++; col = 0; }
        col++;
    }
    *vl = vline; *vc = col;
}
/* visual line & column -> byte offset (clamped to line/doc end) */
static int te_vis_pos(TE_Ctx *c, int vline, int vcol, int W) {
    int vcw = te_vchars(W), vl = 0, col = 0;
    for (int i = 0; i < c->len; i++) {
        if (vl == vline && col == vcol) return i;
        if (vl > vline) return i;      /* ran past target line: clamp to its end */
        if (c->text[i] == '\n') { vl++; col = 0; continue; }
        if (col >= vcw) { vl++; col = 0; }
        col++;
    }
    return c->len;
}

/* ---- "select directory" picker (untitled doc save) ---- */
/* names[0] = "" (Desktop root); then every root-level folder */
static int te_pick_dirs(char names[][VFS_NAME_LEN + 1]) {
    int n = 0;
    names[n][0] = 0; n++;
    int vc = vfs_count();
    for (int i = 0; i < vc && n < 12; i++) {
        const VfsEntry *e = vfs_entry(i);
        /* user folders only: system/protected dirs can't accept saves */
        if (e && e->type == VFS_DIR && !e->system) {
            strncpy(names[n], e->name, VFS_NAME_LEN);
            names[n][VFS_NAME_LEN] = 0;
            n++;
        }
    }
    return n;
}

#define PICK_W 320
#define PICK_NAME_Y 32
#define PICK_NAME_H 22
#define PICK_LIST_Y 76
#define PICK_ROW_H 18
#define PICK_VIS 7                    /* max visible folder rows (scrolls) */
static int pick_h(int n) {
    int vis = n < PICK_VIS ? n : PICK_VIS;
    return PICK_LIST_Y + vis * PICK_ROW_H + 42;
}
/* first visible row so pick_sel stays on screen */
static int pick_start(TE_Ctx *c, int n) {
    int vis = n < PICK_VIS ? n : PICK_VIS;
    int s = c->pick_sel - vis + 1;
    if (s < 0) s = 0;
    if (s + vis > n) s = n - vis;
    if (s < 0) s = 0;
    return s;
}

/* picker hit id: -1 none, -2 name field, 1000=Save, 1001=Cancel,
 * >=0 = folder row index. Same geometry the draw code uses. */
static int pick_id(int px, int py, int pw, int ph, int n,
                   int vstart, int vis, int x, int y) {
    if (x >= px + 60 && x < px + pw - 14 &&
        y >= py + PICK_NAME_Y && y < py + PICK_NAME_Y + PICK_NAME_H)
        return -2;
    if (x >= px + pw - 90 && x < px + pw - 14 &&
        y >= py + ph - 34 && y < py + ph - 8) return 1001;   /* Cancel */
    if (x >= px + 14 && x < px + 90 &&
        y >= py + ph - 34 && y < py + ph - 8) return 1000;   /* Save */
    int vrow = (y - (py + PICK_LIST_Y)) / PICK_ROW_H;
    if (vrow >= 0 && vrow < vis) {
        int ti = vstart + vrow;
        if (ti < n) return ti;
    }
    return -1;
}

/* insert char into pick_name at cursor */
static void pick_insert(TE_Ctx *c, char ch) {
    size_t l = strlen(c->pick_name);
    if (l >= sizeof(c->pick_name) - 1) return;
    memmove(c->pick_name + c->pick_cur + 1, c->pick_name + c->pick_cur, l - c->pick_cur + 1);
    c->pick_name[c->pick_cur++] = ch;
}
static void pick_backspace(TE_Ctx *c) {
    if (c->pick_cur <= 0) return;
    size_t l = strlen(c->pick_name);
    memmove(c->pick_name + c->pick_cur - 1, c->pick_name + c->pick_cur, l - c->pick_cur + 1);
    c->pick_cur--;
}
static void pick_key(TE_Ctx *c, int key) {
    size_t l = strlen(c->pick_name);
    if (key == KEY_LEFT) { if (c->pick_cur > 0) c->pick_cur--; return; }
    if (key == KEY_RIGHT) { if (c->pick_cur < (int)l) c->pick_cur++; return; }
    if (key == KEY_HOME) { c->pick_cur = 0; return; }
    if (key == KEY_END) { c->pick_cur = (int)l; return; }
    if (key == KEY_UP || key == KEY_DOWN) {
        /* arrows also walk the folder list */
        char names[16][VFS_NAME_LEN + 1];
        int n = te_pick_dirs(names);
        c->pick_sel += (key == KEY_DOWN ? 1 : -1);
        if (c->pick_sel < 0) c->pick_sel = 0;
        if (c->pick_sel >= n) c->pick_sel = n - 1;
        return;
    }
}

static void te_draw_pick(TE_Ctx *c, int W, int H) {
    char names[16][VFS_NAME_LEN + 1];
    int n = te_pick_dirs(names);
    int pw = PICK_W, ph = pick_h(n);
    int px = (W - pw) / 2, py = (H - ph) / 2;
    if (py < 8) py = 8;
    app_blend(px + 4, py + 4, pw, ph, rgba(0, 0, 0, 110));
    app_rounded(px, py, pw, ph, 8, rgba(44, 13, 48, 240));
    app_rect(px, py, pw, ph, rgb(202, 110, 220));
    app_text(px + 14, py + 10, "Save As", rgb(249, 225, 255), 1);

    /* name field: click to focus, type to rename */
    app_text(px + 14, py + PICK_NAME_Y - 3, "Name:", rgb(213, 170, 220), 1);
    app_rounded(px + 60, py + PICK_NAME_Y, pw - 74, PICK_NAME_H, 5,
                rgb(238, 240, 248));
    app_rect(px + 60, py + PICK_NAME_Y, pw - 74, PICK_NAME_H,
             c->pick_name_foc ? rgb(138, 0, 255) : rgb(190, 190, 190));
    /* scroll the name so the cursor stays visible (~9px per char) */
    int start = 0;
    if (c->pick_cur > 22) start = c->pick_cur - 22;
    char shown[48];
    strncpy(shown, c->pick_name + start, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = 0;
    app_text_clip(px + 66, py + PICK_NAME_Y + 6, shown, rgb(30, 30, 30), 1,
                  px + 60, py + PICK_NAME_Y, pw - 74, PICK_NAME_H);
    if (c->pick_name_foc && (gui_ticks() / 450) % 2 == 0) {
        char pre[48];
        int pl = c->pick_cur - start; if (pl < 0) pl = 0; if (pl > 47) pl = 47;
        memcpy(pre, shown, pl); pre[pl] = 0;
        int cx = px + 66 + app_text_width(pre, 1);
        int maxx = px + 60 + pw - 74 - 4;
        if (cx > maxx) cx = maxx;
        app_fill(cx, py + PICK_NAME_Y + 3, 2, 16, rgb(30, 30, 30));
    }

    app_text(px + 14, py + PICK_LIST_Y - 16, "Save in:", rgb(213, 170, 220), 1);
    int vstart = pick_start(c, n);
    int vis = n < PICK_VIS ? n : PICK_VIS;
    for (int i = 0; i < vis; i++) {
        int ti = vstart + i;
        if (ti >= n) break;
        int ry = py + PICK_LIST_Y + i * PICK_ROW_H;
        bool sel = (ti == c->pick_sel);
        bool hov = (ti == c->pick_hov), prs = (ti == c->pick_prs);
        if (sel) app_fill(px + 8, ry - 1, pw - 16, PICK_ROW_H - 2, rgba(150, 70, 170, 170));
        else if (prs) app_fill(px + 8, ry - 1, pw - 16, PICK_ROW_H - 2, rgba(105, 50, 120, 150));
        else if (hov) app_fill(px + 8, ry - 1, pw - 16, PICK_ROW_H - 2, rgba(172, 90, 190, 90));
        app_rect(px + 8, ry - 1, pw - 16, PICK_ROW_H - 2,
                 hov || prs ? rgb(182, 110, 200) : rgb(111, 60, 130));
        const char *lbl = names[ti][0] ? names[ti] : "Desktop";
        app_text(px + 16, ry + 4, lbl, sel ? rgb(255, 255, 255) : rgb(233, 200, 240), 1);
    }
    int by = py + ph - 34;
    bool shov = (c->pick_hov == 1000), sprs = (c->pick_prs == 1000);
    bool chov = (c->pick_hov == 1001), cprs = (c->pick_prs == 1001);
    app_rounded(px + 14, by, 76, 26, 6,
                sprs ? rgb(116, 46, 130) : shov ? rgb(188, 96, 210) : rgb(150, 70, 170));
    app_rect(px + 14, by, 76, 26, rgb(124, 50, 140));
    app_text(px + 14 + (76 - app_text_width("Save", 1)) / 2, by + 8,
             "Save", rgb(240, 244, 255), 1);
    app_rounded(px + pw - 90, by, 76, 26, 6,
                cprs ? rgb(95, 40, 110) : chov ? rgb(172, 96, 190) : rgba(132, 60, 150, 170));
    app_rect(px + pw - 90, by, 76, 26, rgb(140, 70, 160));
    app_text(px + pw - 90 + (76 - app_text_width("Cancel", 1)) / 2, by + 8,
             "Cancel", rgb(245, 220, 250), 1);
}

static void te_draw(void *ctx) {
    TE_Ctx *c = (TE_Ctx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(255, 255, 255));
    /* text area */
    app_fill(TE_PAD, TE_PAD, W - 2 * TE_PAD, H - 2 * TE_PAD, rgb(255, 255, 255));
    app_rect(TE_PAD, TE_PAD, W - 2 * TE_PAD, H - 2 * TE_PAD, rgb(210, 210, 210));

    /* draw text as wrapped visual lines (soft-wrap like Notepad) */
    int vcw = te_vchars(W);
    int max_lines = (H - 2 * TE_PAD - 12) / TE_LH + 1;
    int y = TE_PAD + 6 - c->scroll * TE_LH;
    int i = 0, vl = 0, shown = 0;
    while (i < c->len && shown < max_lines + c->scroll + 2) {
        /* gather one visual line: up to vcw chars or until newline */
        char tmp[160];
        int n = 0;
        while (i < c->len && c->text[i] != '\n' && n < vcw && n < 150)
            tmp[n++] = c->text[i++];
        tmp[n] = 0;
        int had_nl = (i < c->len && c->text[i] == '\n');
        if (had_nl) i++;
        if (vl >= c->scroll && y < H - TE_PAD && shown < max_lines) {
            app_text_clip(TE_PAD + 6, y, tmp, rgb(30, 30, 30), 1,
                          TE_PAD, TE_PAD, W - 2 * TE_PAD, H - 2 * TE_PAD);
            y += TE_LH;
            shown++;
        }
        vl++;
        if (i >= c->len) break;
        (void)had_nl;
    }

    /* caret: blinking block at the cursor's WRAPPED position */
    if (c->editing && gui_win_focused() && ((gui_ticks() / 500) & 1) == 0) {
        int cvl, cvc;
        te_pos_vis(c, c->cursor, W, &cvl, &cvc);
        if (cvl >= c->scroll) {
            int cyy = TE_PAD + 6 + (cvl - c->scroll) * TE_LH;
            int cxx = TE_PAD + 6 + cvc * 9;
            if (cyy < H - TE_PAD)
                app_fill(cxx, cyy, 2, 9, rgb(0, 0, 0));
        }
    }
    if (c->pick_active) te_draw_pick(c, W, H);
    /* transient status banner ("This action is denied!") */
    if (c->msg[0]) {
        if (gui_ticks() - c->msg_t > 2500) c->msg[0] = 0;
        else {
            int mw = app_text_width(c->msg, 1) + 28;
            int mx = (W - mw) / 2, my = H - 40;
            app_blend(mx + 3, my + 3, mw, 30, rgba(0, 0, 0, 90));
            app_rounded(mx, my, mw, 30, 6, rgba(44, 13, 48, 235));
            app_rect(mx, my, mw, 30, rgb(182, 96, 200));
            app_text(mx + 14, my + 10, c->msg, rgb(255, 210, 210), 1);
        }
    }
}

/* map a click position to a byte offset in the text */
static void te_click_pos(TE_Ctx *c, int x, int y) {
    int W = app_content_w(), H = app_content_h();
    if (x < TE_PAD || y < TE_PAD || x >= W - TE_PAD || y >= H - TE_PAD) {
        c->editing = false;
        return;
    }
    c->editing = true;
    /* click maps to a WRAPPED visual line/col, then back to byte offset */
    int vline = c->scroll + (y - TE_PAD - 6) / TE_LH;
    int vcol = (x - TE_PAD - 6) / 9;
    int vcw = te_vchars(W);
    if (vcol < 0) vcol = 0;
    if (vcol > vcw) vcol = vcw;
    c->cursor = te_vis_pos(c, vline, vcol, W);
}

static void te_event(void *ctx, GuiEvent *ev) {
    TE_Ctx *c = (TE_Ctx *)ctx;
    /* directory+name picker modal: eats all input while open */
    if (c->pick_active) {
        int W = app_content_w(), H = app_content_h();
        char names[16][VFS_NAME_LEN + 1];
        int n = te_pick_dirs(names);
        int pw = PICK_W, ph = pick_h(n);
        int px = (W - pw) / 2, py = (H - ph) / 2;
        if (py < 8) py = 8;
        if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
            int vstart0 = pick_start(c, n);
            int vis0 = n < PICK_VIS ? n : PICK_VIS;
            c->pick_prs = pick_id(px, py, pw, ph, n, vstart0, vis0,
                                  ev->x, ev->y);
            /* name field: focus + position cursor */
            if (ev->x >= px + 60 && ev->x < px + pw - 14 &&
                ev->y >= py + PICK_NAME_Y && ev->y < py + PICK_NAME_Y + PICK_NAME_H) {
                c->pick_name_foc = true;
                int col = (ev->x - (px + 66)) / 9;
                int start = c->pick_cur > 22 ? c->pick_cur - 22 : 0;
                int l = (int)strlen(c->pick_name);
                c->pick_cur = col + start;
                if (c->pick_cur < 0) c->pick_cur = 0;
                if (c->pick_cur > l) c->pick_cur = l;
                return;
            }
            c->pick_name_foc = false;
            if (ev->x >= px + pw - 90 && ev->x < px + pw - 14 &&
                ev->y >= py + ph - 34 && ev->y < py + ph - 8) {
                c->pick_active = false;              /* Cancel */
                return;
            }
            if (ev->x >= px + 14 && ev->x < px + 90 &&
                ev->y >= py + ph - 34 && ev->y < py + ph - 8) {
                /* Save: write into selected dir with the typed name,
                 * then close the editor window entirely */
                const char *dir = names[c->pick_sel][0] ? names[c->pick_sel] : "";
                if (te_do_save(c, dir, c->pick_name) == 0) {
                    c->pick_active = false;
                    gui_close_current_window();
                } else {
                    c->pick_active = false;   /* failed: back to editing */
                }
                return;
            }
            int vrow = (ev->y - (py + PICK_LIST_Y)) / PICK_ROW_H;
            int vstart = pick_start(c, n);
            int vis = n < PICK_VIS ? n : PICK_VIS;
            if (vrow >= 0 && vrow < vis) {
                int ti = vstart + vrow;
                if (ti < n) { c->pick_sel = ti; c->pick_name_foc = false; }
            }
            return;
        }
        if (ev->type == EV_MOUSE_UP) { c->pick_prs = -1; return; }
        if (ev->type == EV_MOUSE_MOVE) {
            /* Save As picker: I-beam on the name field, hand on every
             * clickable row/button, arrow elsewhere - plus hover/press
             * highlight state so every control shows feedback */
            int pw = PICK_W, ph = pick_h(n);
            int px = (W - pw) / 2, py = (H - ph) / 2;
            if (py < 8) py = 8;
            int vstart = pick_start(c, n);
            int vis = n < PICK_VIS ? n : PICK_VIS;
            int id = pick_id(px, py, pw, ph, n, vstart, vis, ev->x, ev->y);
            c->pick_hov = id;
            if (id == -2) { gui_set_cursor(CUR_IBEAM); return; }
            gui_set_cursor(id >= 0 ? CUR_HAND : -1);
            return;
        }
        if (ev->type == EV_KEY_CHAR) {
            if (ev->key == 8) { pick_backspace(c); return; }
            if (ev->key == 10) {   /* Enter = Save */
                const char *dir = names[c->pick_sel][0] ? names[c->pick_sel] : "";
                if (te_do_save(c, dir, c->pick_name) == 0) {
                    c->pick_active = false;
                    gui_close_current_window();
                } else c->pick_active = false;
                return;
            }
            if (ev->key == 27) { c->pick_active = false; return; }   /* Esc */
            /* typing only goes into the name box while it has focus */
            if (!c->pick_name_foc) return;
            if (ev->key >= 32 && ev->key < 127) { pick_insert(c, (char)ev->key); return; }
            return;
        }
        if (ev->type == EV_KEY_SPECIAL) { pick_key(c, ev->key); return; }
        return;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        te_click_pos(c, ev->x, ev->y);
        return;
    }
    if (ev->type == EV_MOUSE_MOVE) {
        gui_set_cursor(c->editing ? CUR_IBEAM : -1);
        return;
    }
    if (ev->type == EV_MOUSE_WHEEL) {
        c->scroll += ev->wheel > 0 ? -3 : 3;
        if (c->scroll < 0) c->scroll = 0;
        return;
    }
    if (!c->editing) return;   /* not editing until clicked into the text */
    if (ev->type == EV_KEY_CHAR) {
        if (c->read_only) { te_deny(c); return; }   /* view-only system file */
        if (ev->key == 8) { te_backspace(c); return; }
        if (ev->key == 10) { te_newline(c); return; }
        if (ev->key == 9) { te_insert(c, ' '); te_insert(c, ' '); te_insert(c, ' '); te_insert(c, ' '); return; }
        if (ev->key >= 32 && ev->key < 127) te_insert(c, (char)ev->key);
    }
    if (ev->type == EV_KEY_SPECIAL) {
        if (ev->key == KEY_LEFT && c->cursor > 0) c->cursor--;
        else if (ev->key == KEY_RIGHT && c->cursor < c->len) c->cursor++;
        else if (ev->key == KEY_UP || ev->key == KEY_DOWN) {
            /* move by WRAPPED visual line, keeping the column */
            int W = app_content_w();
            int vl, vc;
            te_pos_vis(c, c->cursor, W, &vl, &vc);
            int nvl = vl + (ev->key == KEY_DOWN ? 1 : -1);
            if (nvl < 0) nvl = 0;
            int ncur = te_vis_pos(c, nvl, vc, W);
            if (ev->key == KEY_DOWN && ncur == c->cursor && nvl > vl) {
                /* already at last visual line: try next logical line */
            } else c->cursor = ncur;
        }
        else if (ev->key == KEY_HOME) {
            while (c->cursor > 0 && c->text[c->cursor - 1] != '\n') c->cursor--;
        }
        else if (ev->key == KEY_END) {
            while (c->cursor < c->len && c->text[c->cursor] != '\n') c->cursor++;
        }
    }
}
static void te_update(void *ctx) {
    TE_Ctx *c = (TE_Ctx *)ctx;
    /* autosave dirty NAMED docs ~2x/sec; untitled docs wait for the
     * "Do you want to save?" -> directory picker flow instead */
    static int tick;
    if (c->dirty && c->file[0] && ++tick >= 30) { tick = 0; te_save(c); }
}

static App s_app = {
    "Quill", "Quill", 560, 420, -1,
    te_create, te_destroy, te_init, te_draw, te_event, te_update, 0, 0,
    te_confirm, te_save_now
};
App *make_quill(void) { return &s_app; }
