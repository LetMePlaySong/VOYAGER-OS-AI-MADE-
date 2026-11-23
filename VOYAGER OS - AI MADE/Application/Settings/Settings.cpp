/* Settings.app - system preferences: wallpaper picker, about, user */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../gui/gui.h"

/* full-res photos for the desktop, tiny thumbs for the picker grid -
 * decoding a 3MB photo just to draw a 112px thumbnail would be wasteful */
static const char *kWallpapers[] = {
    "wp_chrysanthemum", "wp_desert", "wp_hydrangeas", "wp_jellyfish",
    "wp_koala", "wp_lighthouse", "wp_penguins", "wp_tulips", "wp_stars"
};
static const char *kWpThumbs[] = {
    "wpt_chrysanthemum", "wpt_desert", "wpt_hydrangeas", "wpt_jellyfish",
    "wpt_koala", "wpt_lighthouse", "wpt_penguins", "wpt_tulips", "wpt_stars"
};
static const char *kWpNames[] = {
    "Chrysanthemum", "Desert", "Hydrangeas", "Jellyfish",
    "Koala", "Lighthouse", "Penguins", "Tulips", "Starfield"
};
#define N_WP 9
#define WP_COLS 4
#define TABBAR_H 30
#define OFF TABBAR_H   /* settings content sits below the tab bar */

/* clipped + loop marquee for labels that can exceed their cell -
 * scrolls left, holds the tail, wraps (same escalator as Finder) */
static void label_marquee(const char *s, int x, int y, int maxw,
                          uint32_t color) {
    int tw = app_text_width(s, 1);
    int dx = x;
    if (tw > maxw && maxw > 30) {
        int travel = tw - maxw + 16;
        int cyc = travel + 120;
        int p = (int)((gui_ticks() / 35) % cyc);
        if (p <= travel) dx = x - p;
        else dx = x - travel;
    }
    app_text_clip(dx, y, s, color, 1, x, y - 2, maxw, 14);
}

typedef struct {
    int wp_sel;
    int hov, prs;   /* hovered/pressed thumbnail index, -1 none */
    char user[48];
    int edit;      /* name box has keyboard focus -> blinking cursor */
    int ucur;      /* cursor byte offset inside user[] */
    int tab;       /* 0 = Settings, 1 = Date & Time, 2 = Task Manager */
    int tab_fx, tab_fw;      /* pill rect the selection slid FROM */
    uint64_t tab_anim_t;     /* nonzero while the pill glides (ms) */
    int tm_scroll; /* task-manager list scroll offset (rows) */
    int tm_hov;    /* hovered "End Task" row, -1 none */
    int tm_prs;    /* pressed "End Task" row, -1 none */
    int tab_hov, tab_prs;  /* hovered/pressed tab pill, -1 none */
    int clk_hov, clk_prs;  /* Date&Time control: 0=tz< 1=tz> 2=sync
                              3=h- 4=h+ 5=m- 6=m+, -1 none */
} SetCtx;

static void *set_create(void) {
    SetCtx *c = (SetCtx *)kzalloc(sizeof(SetCtx));
    c->hov = -1; c->prs = -1; c->edit = 1; c->tm_hov = -1;
    c->tm_prs = -1; c->tab_hov = c->tab_prs = -1;
    c->clk_hov = c->clk_prs = -1;
    c->ucur = 0;
    strncpy(c->user, gui_user_name(), sizeof(c->user) - 1);
    c->ucur = (int)strlen(c->user);
    /* highlight whichever wallpaper is actually on the desktop */
    const char *cur = gui_wallpaper_name();
    c->wp_sel = 0;
    for (int i = 0; i < N_WP; i++)
        if (cur && !strcmp(kWallpapers[i], cur)) c->wp_sel = i;
    return c;
}
static void set_init(void *ctx) { (void)ctx; }
static void set_destroy(void *ctx) { kfree(ctx); }

extern "C" int settings_save(const char *, const char *);
static void save_settings(SetCtx *c) {
    settings_save(kWallpapers[c->wp_sel], c->user);
}

/* tab strip: [Settings] [Task Manager] - Voyager-System-Preferences style */
/* timezone picker: name, minutes east of UTC, DST rule (0/1US/2EU) */
typedef struct { const char *name; int16_t min; uint8_t dst; } TzZone;
static const TzZone kZones[] = {
    { "UTC",                    0, 0 },
    { "US Eastern",          -300, 1 },
    { "US Central",          -360, 1 },
    { "US Mountain",         -420, 1 },
    { "US Pacific",          -480, 1 },
    { "US Alaska",           -540, 1 },
    { "US Hawaii",           -600, 0 },
    { "GMT/London",            60, 2 },
    { "Central Europe",       120, 2 },
    { "Eastern Europe",       180, 2 },
    { "India",                330, 0 },
    { "China",                480, 0 },
    { "Japan",                540, 0 },
    { "Australia East",       600, 3 },
    { "Australia West",       480, 0 },
    { "New Zealand",          780, 2 },
};
#define N_ZONES ((int)(sizeof(kZones)/sizeof(kZones[0])))

extern "C" void settings_get_tz(int16_t *, uint8_t *, uint8_t *);
extern "C" int  settings_set_tz(int16_t, uint8_t);

static int tz_index(void) {
    int16_t mn; uint8_t ru, fl;
    settings_get_tz(&mn, &ru, &fl);
    for (int i = 0; i < N_ZONES; i++)
        if (kZones[i].min == mn && kZones[i].dst == ru) return i;
    return -1;   /* custom/unsaved combo -> show raw offset */
}
static void tz_cycle(int dir) {
    int i = tz_index();
    i = (i < 0 ? 0 : i) + dir;
    if (i < 0) i = N_ZONES - 1;
    if (i >= N_ZONES) i = 0;
    settings_set_tz(kZones[i].min, kZones[i].dst);
}
static void tz_label(char *buf, size_t cap) {
    int i = tz_index();
    if (i >= 0) { strncpy(buf, kZones[i].name, cap - 1); buf[cap-1] = 0; return; }
    int16_t mn; uint8_t ru, fl;
    settings_get_tz(&mn, &ru, &fl);
    /* custom snprintf has no flag/width support - build "UTC+5[:30]"
     * by hand so no format specifier can misparse */
    int h = mn / 60, r = mn % 60; if (r < 0) r = -r; if (h < 0) h = -h;
    char sign = mn < 0 ? '-' : '+';
    char tmp[24]; int ti = 0;
    tmp[ti++]='U'; tmp[ti++]='T'; tmp[ti++]='C'; tmp[ti++]=sign;
    tmp[ti++]='0'+h/10; tmp[ti++]='0'+h%10;
    if (r) { tmp[ti++]=':'; tmp[ti++]='0'+r/10; tmp[ti++]='0'+r%10; }
    tmp[ti]=0;
    strncpy(buf, tmp, cap - 1); buf[cap-1] = 0;
}
/* < > button rects in content coords (set in draw, used by event) */
static int g_tz_lx, g_tz_rx, g_tz_y, g_tz_w;
/* manual clock controls (set in draw, used by event) */
static int g_clk_sync[4];    /* Sync Now button */
static int g_clk_hdec[4], g_clk_hval[4], g_clk_hinc[4];
static int g_clk_mdec[4], g_clk_mval[4], g_clk_minc[4];

/* pill rect for tab i in content coords - shared by draw + hit test so
 * the animated selection can glide between exact tab bounds */
static void tab_pill_rect(int i, int *px, int *pw) {
    static const char *names[] = { "Settings", "Date & Time", "Task Manager" };
    int x = 14;
    for (int k = 0; k < i; k++)
        x += app_text_width(names[k], 1) + 20 + 8;
    *px = x; *pw = app_text_width(names[i], 1) + 20;
}

static void draw_tabbar(SetCtx *c, int W) {
    static const char *names[] = { "Settings", "Date & Time", "Task Manager" };
    /* selection pill GLIDES from the old tab to the new one (~200ms
     * ease-out) instead of snapping - the premium tab feel */
    int anim = 0, ax = 0, aw = 0;
    if (c->tab_anim_t) {
        uint64_t el = gui_ticks() - c->tab_anim_t;
        if (el >= 200) c->tab_anim_t = 0;
        else {
            anim = 1;
            int tx, tw2; tab_pill_rect(c->tab, &tx, &tw2);
            int e = 100 - (int)el;             /* 200 -> 0 */
            int t = (int)(el * 100 / 200);     /* 0..99 */
            int ee = 100 - (e * e) / 100;      /* ease-out 0..100 */
            (void)t;
            ax = c->tab_fx + (tx - c->tab_fx) * ee / 100;
            aw = c->tab_fw + (tw2 - c->tab_fw) * ee / 100;
            if (aw < 10) aw = 10;
        }
    }
    int x = 14;
    for (int i = 0; i < 3; i++) {
        int tw = app_text_width(names[i], 1) + 20;
        bool sel = c->tab == i && !anim;
        uint32_t face;
        uint32_t tcol;
        if (sel)                { face = rgb(138, 0, 255); tcol = rgb(255, 255, 255); }
        else if (c->tab_prs == i) { face = rgb(198, 200, 212); tcol = rgb(70, 70, 80); }
        else if (c->tab_hov == i) { face = rgb(238, 240, 248); tcol = rgb(40, 40, 55); }
        else                    { face = rgb(226, 226, 232); tcol = rgb(70, 70, 80); }
        app_rounded(x, 5, tw, TABBAR_H - 10, 6, face);
        app_text(x + 10, 11, names[i], tcol, 1);
        x += tw + 8;
    }
    if (anim) {
        /* the traveling selection pill - same violet face + white text */
        int ti = c->tab;
        int ty = 5;
        app_rounded(ax, ty, aw, TABBAR_H - 10, 6, rgb(138, 0, 255));
        /* keep the label centered in the traveling pill */
        int tl = app_text_width(names[ti], 1);
        app_text(ax + (aw - tl) / 2, 11, names[ti], rgb(255, 255, 255), 1);
    }
    app_fill(0, TABBAR_H + 4, W, 1, rgb(210, 210, 216));
}

static int tab_hit(int x, int y) {
    if (y < 5 || y >= TABBAR_H - 5) return -1;
    for (int i = 0; i < 3; i++) {
        int px, pw; tab_pill_rect(i, &px, &pw);
        if (x >= px && x < px + pw) return i;
    }
    return -1;
}

/* ---- Task Manager pane: memory meter + window list with End Task ---- */
#define TM_ROW_H 42
#define TM_END_W 96
#define TM_BTN_H 20

static int tm_rows(void) { return gui_win_list_count(); }

static void draw_taskmgr(SetCtx *c, int W, int H) {
    int y = TABBAR_H + 14;
    /* memory meter */
    size_t used = kmalloc_used(), total = kmalloc_total();
    int pct = total ? (int)(used * 100 / total) : 0;
    app_text(20, y, "Memory", rgb(30, 30, 30), 2);
    char mb[64];
    snprintf(mb, sizeof(mb), "%u / %u MB used (%d%%)",
             (unsigned)(used >> 20), (unsigned)(total >> 20), pct);
    app_text(20, y + 26, mb, rgb(90, 90, 90), 1);
    int barx = 20, barw = W - 40, bary = y + 48, barh = 14;
    app_rounded(barx, bary, barw, barh, 7, rgb(60, 60, 70));
    int fillw = barw * pct / 100;
    uint32_t fc = pct < 70 ? rgb(80, 200, 120) :
                  pct < 88 ? rgb(240, 170, 40) : rgb(230, 60, 60);
    if (fillw > 4) app_rounded(barx + 2, bary + 2, fillw - 4, barh - 4, 6, fc);
    app_text_clip(barx, bary + barh + 8,
                  "Close apps or tabs to free memory.",
                  rgb(110, 110, 120), 1, 16, bary + barh + 4, W - 32, 16);
    app_text_clip(barx, bary + barh + 24,
                  "The system warns and closes app!",
                  rgb(110, 110, 120), 1, 16, bary + barh + 20, W - 32, 16);

    /* window list */
    int ly = bary + barh + 48;
    app_text(20, ly, "Applications", rgb(30, 30, 30), 2);
    ly += 26;
    int n = tm_rows();
    int rowh = TM_ROW_H;
    int visible = (H - ly - 10) / rowh;
    if (visible < 1) visible = 1;
    if (c->tm_scroll > n - visible) c->tm_scroll = n - visible;
    if (c->tm_scroll < 0) c->tm_scroll = 0;
    for (int i = c->tm_scroll; i < n && i < c->tm_scroll + visible; i++) {
        int ry = ly + (i - c->tm_scroll) * rowh;
        char an[32], tt[64];
        int foc = 0, min = 0;
        int id = gui_win_list_get(i, an, sizeof(an), tt, sizeof(tt), &foc, &min);
        bool hovend = (c->tm_hov == id);
        bool prsend = (c->tm_prs == id);
        app_fill(20, ry, W - 40, rowh - 4,
                 prsend ? rgb(226, 200, 245) :
                 hovend ? rgb(241, 225, 250) : rgb(238, 238, 242));
        app_rect(20, ry, W - 40, rowh - 4, rgb(215, 215, 222));
        /* status dot: green=focused blue=normal gray=minimized */
        uint32_t dot = foc ? rgb(60, 190, 90) : min ? rgb(160, 160, 170) : rgb(173, 70, 230);
        app_fill(30, ry + (rowh - 4 - 10) / 2, 10, 10, dot);
        /* two-line row: app name on top, window title below - both
         * ellipsized to the space left of End Task so nothing is ever
         * sliced mid-glyph or spilled into the button */
        int ex = W - 40 - TM_END_W - 14;
        int txw = ex - 56 - 10;          /* text column width */
        if (txw < 40) txw = 40;
        char disp[48], tdisp[80];
        gui_fit_label(disp, sizeof(disp), an, txw);
        gui_fit_label(tdisp, sizeof(tdisp), tt, txw);
        app_text(50, ry + 4, disp, rgb(30, 30, 30), 1);
        app_text(50, ry + 21, tdisp, rgb(100, 100, 110), 1);
        int ey = ry + (rowh - TM_BTN_H) / 2;
        uint32_t bf = prsend ? rgb(190, 60, 52) :
                      hovend ? rgb(220, 90, 80) : rgb(235, 120, 110);
        app_rounded(ex, ey, TM_END_W, TM_BTN_H, 5, bf);
        app_text(ex + (TM_END_W - app_text_width("End Task", 1)) / 2,
                 ey + 5, "End Task", rgb(255, 255, 255), 1);
    }
    if (n == 0)
        app_text(20, ly, "No open windows.", rgb(120, 120, 130), 1);
    /* scroll hint when the list overflows */
    if (n > visible)
        app_text(20, H - 24, "Scroll to see more", rgb(140, 140, 150), 1);
}

/* Date & Time pane: timezone picker (auto DST), live clock, Sync Now,
 * and manual hour/minute steppers - its own tab so the Settings tab
 * stays compact at the fixed window size */
/* frosted grouped card - Voyager-style section panel */
static void pref_card(int x, int y, int w, int h) {
    app_rounded(x, y, w, h, 10, rgb(252, 252, 254));
    app_blend(x + 1, y + 1, w - 2, h - 2, rgba(255, 255, 255, 120));
    app_rect(x, y, w, h, rgb(206, 209, 216));
}
static void pref_label(int x, int y, const char *s) {
    app_text(x, y, s, rgb(108, 96, 112), 1);
}

/* one Date & Time button: hover = lighter face, press = deeper face,
 * same rounded+border treatment as every other Settings control */
static void clk_btn(SetCtx *c, int id, int x, int y, int w, int h,
                    const char *lbl, int primary) {
    bool hov = (c->clk_hov == id), prs = (c->clk_prs == id);
    uint32_t face, border, tcol;
    if (primary) {
        face   = prs ? rgb(113, 0, 205)  : hov ? rgb(167, 46, 255) : rgb(138, 0, 255);
        border = rgb(104, 0, 190);
        tcol   = rgb(255, 255, 255);
    } else {
        face   = prs ? rgb(225, 196, 238) : hov ? rgb(241, 222, 250) : rgb(238, 239, 244);
        border = prs || hov ? rgb(191, 140, 210) : rgb(178, 181, 190);
        tcol   = rgb(50, 50, 60);
    }
    app_rounded(x, y, w, h, 6, face);
    app_rect(x, y, w, h, border);
    int lw = app_text_width(lbl, 1);
    app_text(x + (w - lw) / 2, y + (h - 9) / 2 + 1, lbl, tcol, 1);
}

static void draw_datetime(SetCtx *c, int W, int H) {
    (void)H;
    int m = 20, cw = W - 40;
    int y = TABBAR_H + 14;
    app_text(m, y, "Date & Time", rgb(30, 30, 30), 2);
    y += 30;

    /* ---- Time Zone card ---- */
    int ch = 86, cy = y;
    pref_card(m, cy, cw, ch);
    pref_label(m + 14, cy + 10, "Time Zone");
    app_text(m + 14, cy + 32, "Daylight saving adjusts automatically",
             rgb(140, 142, 150), 1);
    char tl[48]; tz_label(tl, sizeof(tl));
    int bx = m + 14, bw = cw - 14 * 2 - 56;
    app_rounded(bx, cy + 52, bw, 24, 6, rgb(255, 255, 255));
    app_rect(bx, cy + 52, bw, 24, rgb(178, 181, 190));
    app_text_clip(bx + 10, cy + 57, tl, rgb(30, 30, 30), 1,
                  bx, cy + 52, bw, 24);
    g_tz_lx = m + cw - 14 - 48; g_tz_rx = m + cw - 14 - 24;
    g_tz_y = cy + 52; g_tz_w = 24;
    clk_btn(c, 0, g_tz_lx, g_tz_y, g_tz_w, 24, "<", 0);
    clk_btn(c, 1, g_tz_rx, g_tz_y, g_tz_w, 24, ">", 0);
    y += ch + 12;

    /* ---- Clock card: live time + Sync Now ---- */
    ch = 64; cy = y;
    pref_card(m, cy, cw, ch);
    pref_label(m + 14, cy + 10, "Clock");
    int chh, cmi, cs;
    gui_rtc_local_hms(&chh, &cmi, &cs);
    int h12 = chh % 12; if (h12 == 0) h12 = 12;
    /* built by hand: snprintf has no %02d and would shift args */
    char clk[32]; int ci = 0;
    clk[ci++]='0'+h12/10; clk[ci++]='0'+h12%10; clk[ci++]=':';
    clk[ci++]='0'+cmi/10; clk[ci++]='0'+cmi%10; clk[ci++]=':';
    clk[ci++]='0'+cs/10;  clk[ci++]='0'+cs%10;  clk[ci++]=' ';
    clk[ci++]='A'; clk[ci++]='M';
    if (chh >= 12) clk[ci-2]='P';
    clk[ci]=0;
    int cbw = 160;
    app_rounded(bx, cy + 34, cbw, 24, 6, rgb(255, 255, 255));
    app_rect(bx, cy + 34, cbw, 24, rgb(178, 181, 190));
    app_text_clip(bx + 12, cy + 39, clk, rgb(30, 30, 30), 1,
                  bx, cy + 34, cbw, 24);
    g_clk_sync[0] = m + cw - 14 - 96;
    g_clk_sync[1] = cy + 34; g_clk_sync[2] = 96; g_clk_sync[3] = 24;
    /* live status on the button: Sync Now -> Syncing... -> Synced/Failed,
     * so a click always has visible feedback */
    int ss = gui_time_sync_state();
    if (ss == 3) {
        int bx2 = g_clk_sync[0], by2 = g_clk_sync[1];
        app_rounded(bx2, by2, 96, 24, 6, rgb(226, 70, 70));
        app_rect(bx2, by2, 96, 24, rgb(200, 55, 55));
        app_text(bx2 + (96 - app_text_width("Failed", 1)) / 2,
                 by2 + 8, "Failed", rgb(255, 255, 255), 1);
    } else {
        clk_btn(c, 2, g_clk_sync[0], g_clk_sync[1], g_clk_sync[2],
                g_clk_sync[3],
                ss == 1 ? "Syncing..." : ss == 2 ? "Synced" : "Sync Now", 1);
    }
    y += ch + 12;

    /* ---- Set Time card: hour/minute steppers ---- */
    ch = 78; cy = y;
    pref_card(m, cy, cw, ch);
    pref_label(m + 14, cy + 10, "Set Time");
    int sy = cy + 36;
    app_text(m + 14, sy + 7, "Hour", rgb(90, 92, 100), 1);
    g_clk_hdec[0]=m+64;  g_clk_hdec[1]=sy; g_clk_hdec[2]=24; g_clk_hdec[3]=24;
    g_clk_hval[0]=m+92;  g_clk_hval[1]=sy; g_clk_hval[2]=44; g_clk_hval[3]=24;
    g_clk_hinc[0]=m+140; g_clk_hinc[1]=sy; g_clk_hinc[2]=24; g_clk_hinc[3]=24;
    clk_btn(c, 3, g_clk_hdec[0], g_clk_hdec[1], 24, 24, "<", 0);
    app_rounded(g_clk_hval[0], g_clk_hval[1], 44, 24, 6, rgb(255,255,255));
    app_rect(g_clk_hval[0], g_clk_hval[1], 44, 24, rgb(178,181,190));
    { char hv[8]; snprintf(hv, sizeof(hv), "%d", h12);
      app_text(g_clk_hval[0]+16, g_clk_hval[1]+7, hv, rgb(30,30,30), 1); }
    clk_btn(c, 4, g_clk_hinc[0], g_clk_hinc[1], 24, 24, ">", 0);

    int mx = m + 190;
    app_text(mx, sy + 7, "Minute", rgb(90, 92, 100), 1);
    g_clk_mdec[0]=mx+56; g_clk_mdec[1]=sy; g_clk_mdec[2]=24; g_clk_mdec[3]=24;
    g_clk_mval[0]=mx+84; g_clk_mval[1]=sy; g_clk_mval[2]=44; g_clk_mval[3]=24;
    g_clk_minc[0]=mx+132;g_clk_minc[1]=sy; g_clk_minc[2]=24; g_clk_minc[3]=24;
    clk_btn(c, 5, g_clk_mdec[0], g_clk_mdec[1], 24, 24, "<", 0);
    app_rounded(g_clk_mval[0], g_clk_mval[1], 44, 24, 6, rgb(255,255,255));
    app_rect(g_clk_mval[0], g_clk_mval[1], 44, 24, rgb(178,181,190));
    { char mv[8]; mv[0]='0'+cmi/10; mv[1]='0'+cmi%10; mv[2]=0;
      app_text(g_clk_mval[0]+14, g_clk_mval[1]+7, mv, rgb(30,30,30), 1); }
    clk_btn(c, 6, g_clk_minc[0], g_clk_minc[1], 24, 24, ">", 0);
}

static void set_draw(void *ctx) {
    SetCtx *c = (SetCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(245, 245, 247));
    draw_tabbar(c, W);
    if (c->tab == 2) { draw_taskmgr(c, W, H); return; }
    if (c->tab == 1) { draw_datetime(c, W, H); return; }

    int tw = 96, th = 58, gx = 7, gy = 6;
    int x0 = 20, y0 = 64 + OFF;
    /* section card behind the whole wallpaper group */
    int wprows = (N_WP + WP_COLS - 1) / WP_COLS;
    int uy0 = y0 + wprows * (th + gy + 16) + 14;
    pref_card(12, 40, W - 24, uy0 - 18 - 40);
    app_text(28, 18 + OFF, "Desktop & Screen Saver", rgb(30, 30, 30), 2);
    app_text(28, 44 + OFF, "Choose a wallpaper:", rgb(90, 90, 90), 1);
    for (int i = 0; i < N_WP; i++) {
        int ix = x0 + (i % WP_COLS) * (tw + gx);
        int iy = y0 + (i / WP_COLS) * (th + gy + 16);
        app_fill(ix, iy, tw, th, rgb(80, 60, 80));
        uint32_t iw, ih;
        const uint8_t *img = asset_image(kWpThumbs[i], &iw, &ih);
        if (img) {
            /* fit inside the cell preserving aspect - never stretch */
            int aw = tw - 4, ah = th - 4;
            int dw = aw, dh = (int)((int64_t)ah * iw / ih);
            if (dh > ah) { dh = ah; dw = (int)((int64_t)aw * ih / iw); }
            app_draw_image_scaled(ix + 2 + (aw - dw) / 2, iy + 2 + (ah - dh) / 2,
                                  dw, dh, img, iw, ih);
        }
        app_rect(ix, iy, tw, th, i == c->wp_sel ? rgb(138, 0, 255) : rgb(180, 180, 180));
        if (i == c->wp_sel || i == c->hov) app_rect(ix + 2, iy + 2, tw - 4, th - 4, rgb(138, 0, 255));
        /* press = orange tint wash over the thumbnail (darker on hold) */
        if (i == c->prs) app_blend(ix, iy, tw, th, rgba(230, 120, 0, 90));
        label_marquee(kWpNames[i], ix, iy + th + 3, tw, rgb(60, 60, 60));
    }

    /* User - grouped card like the Date & Time pane */
    int uy = y0 + ((N_WP + WP_COLS - 1) / WP_COLS) * (th + gy + 16) + 14;
    int ubw = W - 24 - 32 - 170 - 16;
    pref_card(12, uy - 6, W - 24, 88);
    app_text(32, uy + 12, "User Account", rgb(30, 30, 30), 2);
    app_text(32, uy + 40, "Name:", rgb(90, 90, 90), 1);
    app_rounded(170, uy + 34, ubw, 26, 6, rgb(255, 255, 255));
    app_rect(170, uy + 34, ubw, 26,
             c->edit ? rgb(138, 0, 255) : rgb(190, 190, 190));
    /* clip so long names scroll inside the box instead of overflowing.
     * show the tail of the name when the cursor is near the right edge */
    int ulen = (int)strlen(c->user);
    int start = 0;
    if (c->ucur > 22) start = c->ucur - 22;
    char shown[48];
    strncpy(shown, c->user + start, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = 0;
    app_text_clip(178, uy + 40, shown, rgb(30, 30, 30), 1,
                  170, uy + 34, ubw, 26);
    /* blinking caret at the cursor position while the box is focused */
    if (c->edit && (gui_ticks() / 450) % 2 == 0) {
        char pre[48];
        int pl = c->ucur - start; if (pl < 0) pl = 0; if (pl > 47) pl = 47;
        memcpy(pre, shown, pl); pre[pl] = 0;
        int cx = 178 + app_text_width(pre, 1);
        if (cx > 170 + ubw - 4) cx = 170 + ubw - 4;
        app_fill(cx, uy + 40, 2, 12, rgb(30, 30, 30));
    }

    /* About - grouped card */
    int ay = uy + 96;
    pref_card(12, ay - 8, W - 24, 98);
    app_text(32, ay + 10, "About This Computer", rgb(30, 30, 30), 2);
    app_text(32, ay + 40, "Voyager OS 1.0", rgb(90, 90, 90), 1);
    app_text(32, ay + 58, "Kernel: x86-64, Limine bootloader", rgb(90, 90, 90), 1);
    app_text(32, ay + 76, "GUI: custom window manager", rgb(90, 90, 90), 1);

}

/* hit-test wallpaper thumbnails (content coords) */
static int wp_hit(int x, int y) {
    int tw = 96, th = 58, gx = 7, gy = 6;
    int x0 = 20, y0 = 64 + OFF;
    for (int i = 0; i < N_WP; i++) {
        int ix = x0 + (i % WP_COLS) * (tw + gx);
        int iy = y0 + (i / WP_COLS) * (th + gy + 16);
        if (x >= ix && x < ix + tw && y >= iy && y < iy + th + 16) return i;
    }
    return -1;
}

static int name_hit(int x, int y, int uy) {
    int bw = app_content_w() - 242;
    return x >= 170 && x < 170 + bw && y >= uy + 34 && y < uy + 60;
}

static void set_event(void *ctx, GuiEvent *ev) {
    SetCtx *c = (SetCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    /* tab strip clicks work from EITHER tab - handle before the
     * task-manager early return so Settings <-> Task Manager flip-flops */
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int th = tab_hit(ev->x, ev->y);
        if (th >= 0 && th != c->tab) {
            /* record the pill's current rect so the selection can
             * glide to the new tab instead of snapping */
            tab_pill_rect(c->tab, &c->tab_fx, &c->tab_fw);
            c->tab_anim_t = gui_ticks();
            c->tab_prs = th; c->tab = th; c->edit = 0;
            c->tm_hov = -1; return;
        }
    }
    /* task-manager tab: its own hit zones, no settings editing */
    if (c->tab == 2) {
        /* rows start at the same y draw_taskmgr computes */
        int y = TABBAR_H + 14;
        int ly = y + 48 + 14 + 48 + 26;
        int n = tm_rows();
        int rowh = TM_ROW_H;
        int visible = (H - ly - 10) / rowh;
        if (visible < 1) visible = 1;
        if (ev->type == EV_MOUSE_WHEEL) {
            c->tm_scroll += ev->wheel > 0 ? -3 : 3;
            if (c->tm_scroll < 0) c->tm_scroll = 0;
            if (c->tm_scroll > n - visible) c->tm_scroll = n - visible;
            return;
        }
        if (ev->type == EV_MOUSE_MOVE) {
            /* tab strip hover works on this tab too */
            int thh = tab_hit(ev->x, ev->y);
            c->tab_hov = thh;
            if (thh >= 0) { gui_set_cursor(CUR_HAND); return; }
            c->tm_hov = -1;
            int ex = W - 40 - TM_END_W - 14;
            for (int i = c->tm_scroll; i < n && i < c->tm_scroll + visible; i++) {
                int ry = ly + (i - c->tm_scroll) * rowh;
                int ey = ry + (rowh - TM_BTN_H) / 2;
                if (ev->y >= ey && ev->y < ey + TM_BTN_H &&
                    ev->x >= ex && ev->x < ex + TM_END_W) {
                    char an[32], tt[64]; int f = 0, m = 0;
                    c->tm_hov = gui_win_list_get(i, an, sizeof(an),
                                                 tt, sizeof(tt), &f, &m);
                    break;
                }
            }
            gui_set_cursor(c->tm_hov >= 0 ? CUR_HAND : -1);
            return;
        }
        if (ev->type == EV_MOUSE_UP) { c->tm_prs = -1; c->tab_prs = -1; return; }
        if (ev->type == EV_MOUSE_DOWN && ev->button == 0 && c->tm_hov >= 0) {
            c->tm_prs = c->tm_hov;
            gui_close_window(c->tm_hov);   /* End Task = close that window */
            c->tm_hov = -1;
            return;
        }
        return;
    }
    /* must match set_draw's uy exactly: y0=64+OFF, pitch=th+gy+16=80 */
    int uy = 64 + OFF + ((N_WP + WP_COLS - 1) / WP_COLS) * (58 + 6 + 16) + 14;
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        /* Date & Time tab controls (tab 1) - ignored on the Settings tab
         * so stale rects can't false-trigger */
        if (c->tab == 1) {
        /* timezone < > cycle buttons */
        if (ev->y >= g_tz_y && ev->y < g_tz_y + g_tz_w &&
            ev->x >= g_tz_lx && ev->x < g_tz_lx + g_tz_w) {
            c->clk_prs = 0; tz_cycle(-1); return; }
        if (ev->y >= g_tz_y && ev->y < g_tz_y + g_tz_w &&
            ev->x >= g_tz_rx && ev->x < g_tz_rx + g_tz_w) {
            c->clk_prs = 1; tz_cycle(1); return; }
        /* manual clock controls */
        if (ev->x >= g_clk_sync[0] && ev->x < g_clk_sync[0] + g_clk_sync[2] &&
            ev->y >= g_clk_sync[1] && ev->y < g_clk_sync[1] + g_clk_sync[3]) {
            c->clk_prs = 2; gui_sntp_sync_now(); return;
        }
        {
            int ch, cmi, cs, cid = -1;
            gui_rtc_local_hms(&ch, &cmi, &cs);
            bool hour_hit =
                (ev->x >= g_clk_hdec[0] && ev->x < g_clk_hdec[0] + g_clk_hdec[2] &&
                 ev->y >= g_clk_hdec[1] && ev->y < g_clk_hdec[1] + g_clk_hdec[3]) ||
                (ev->x >= g_clk_hinc[0] && ev->x < g_clk_hinc[0] + g_clk_hinc[2] &&
                 ev->y >= g_clk_hinc[1] && ev->y < g_clk_hinc[1] + g_clk_hinc[3]);
            bool min_hit =
                (ev->x >= g_clk_mdec[0] && ev->x < g_clk_mdec[0] + g_clk_mdec[2] &&
                 ev->y >= g_clk_mdec[1] && ev->y < g_clk_mdec[1] + g_clk_mdec[3]) ||
                (ev->x >= g_clk_minc[0] && ev->x < g_clk_minc[0] + g_clk_minc[2] &&
                 ev->y >= g_clk_minc[1] && ev->y < g_clk_minc[1] + g_clk_minc[3]);
            if (hour_hit || min_hit) {
                bool dec =
                    (ev->x >= g_clk_hdec[0] && ev->x < g_clk_hdec[0] + g_clk_hdec[2] &&
                     ev->y >= g_clk_hdec[1] && ev->y < g_clk_hdec[1] + g_clk_hdec[3]) ||
                    (ev->x >= g_clk_mdec[0] && ev->x < g_clk_mdec[0] + g_clk_mdec[2] &&
                     ev->y >= g_clk_mdec[1] && ev->y < g_clk_mdec[1] + g_clk_mdec[3]);
                if (ev->x >= g_clk_hdec[0] && ev->x < g_clk_hdec[0] + g_clk_hdec[2] &&
                    ev->y >= g_clk_hdec[1] && ev->y < g_clk_hdec[1] + g_clk_hdec[3]) cid = 3;
                else if (ev->x >= g_clk_hinc[0] && ev->x < g_clk_hinc[0] + g_clk_hinc[2] &&
                         ev->y >= g_clk_hinc[1] && ev->y < g_clk_hinc[1] + g_clk_hinc[3]) cid = 4;
                else if (ev->x >= g_clk_mdec[0] && ev->x < g_clk_mdec[0] + g_clk_mdec[2] &&
                         ev->y >= g_clk_mdec[1] && ev->y < g_clk_mdec[1] + g_clk_mdec[3]) cid = 5;
                else cid = 6;
                c->clk_prs = cid;
                if (hour_hit) { ch = (ch + (dec ? 23 : 1)) % 24; }
                else          { cmi = (cmi + (dec ? 59 : 1)) % 60; }
                gui_rtc_set_local(ch, cmi, cs);
                return;
            }
        }
        return;   /* Date & Time tab: nothing else is clickable */
        }
        /* clicking the box focuses it AND positions the cursor by x;
         * clicking elsewhere releases focus */
        c->edit = name_hit(ev->x, ev->y, uy);
        if (c->edit) {
            int col = (ev->x - 178) / 9;   /* ~9px per char at scale 1 */
            if (col < 0) col = 0;
            int l = (int)strlen(c->user);
            if (col > l) col = l;
            c->ucur = col;
            return;
        }
    }
    if (ev->type == EV_KEY_SPECIAL && c->edit) {
        int l = (int)strlen(c->user);
        if (ev->key == KEY_LEFT) { if (c->ucur > 0) c->ucur--; return; }
        if (ev->key == KEY_RIGHT) { if (c->ucur < l) c->ucur++; return; }
        if (ev->key == KEY_HOME || ev->key == KEY_UP) { c->ucur = 0; return; }
        if (ev->key == KEY_END  || ev->key == KEY_DOWN) { c->ucur = l; return; }
        return;
    }
    if (ev->type == EV_MOUSE_MOVE) {
        /* tab strip is clickable from every tab -> hand cursor +
         * hover pill highlight */
        int thh = tab_hit(ev->x, ev->y);
        c->tab_hov = thh;
        if (thh >= 0) { c->hov = -2; gui_set_cursor(CUR_HAND); return; }
        if (c->tab == 1) {
            /* Date & Time controls -> hand cursor + hover face
             * (same rects the click handler uses, same ids draw uses) */
            int id = -1;
            if (ev->y >= g_tz_y && ev->y < g_tz_y + 24) {
                if (ev->x >= g_tz_lx && ev->x < g_tz_lx + 24) id = 0;
                else if (ev->x >= g_tz_rx && ev->x < g_tz_rx + 24) id = 1;
            }
            if (ev->x >= g_clk_sync[0] && ev->x < g_clk_sync[0] + g_clk_sync[2] &&
                ev->y >= g_clk_sync[1] && ev->y < g_clk_sync[1] + g_clk_sync[3]) id = 2;
            if (ev->x >= g_clk_hdec[0] && ev->x < g_clk_hdec[0] + g_clk_hdec[2] &&
                ev->y >= g_clk_hdec[1] && ev->y < g_clk_hdec[1] + g_clk_hdec[3]) id = 3;
            if (ev->x >= g_clk_hinc[0] && ev->x < g_clk_hinc[0] + g_clk_hinc[2] &&
                ev->y >= g_clk_hinc[1] && ev->y < g_clk_hinc[1] + g_clk_hinc[3]) id = 4;
            if (ev->x >= g_clk_mdec[0] && ev->x < g_clk_mdec[0] + g_clk_mdec[2] &&
                ev->y >= g_clk_mdec[1] && ev->y < g_clk_mdec[1] + g_clk_mdec[3]) id = 5;
            if (ev->x >= g_clk_minc[0] && ev->x < g_clk_minc[0] + g_clk_minc[2] &&
                ev->y >= g_clk_minc[1] && ev->y < g_clk_minc[1] + g_clk_minc[3]) id = 6;
            c->clk_hov = id;
            c->hov = -1;
            gui_set_cursor(id >= 0 ? CUR_HAND : -1);
            return;
        }
        c->clk_hov = -1;
        if (c->tab != 0) { c->hov = -1; gui_set_cursor(-1); return; }
        c->hov = wp_hit(ev->x, ev->y);
        if (c->hov >= 0) gui_set_cursor(CUR_HAND);
        else if (c->edit || name_hit(ev->x, ev->y, uy) >= 0)
            gui_set_cursor(CUR_IBEAM);
        else gui_set_cursor(-1);
        return;
    }
    if (ev->type == EV_MOUSE_UP) {
        c->prs = -1; c->clk_prs = -1; c->tab_prs = -1; return;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int hit = wp_hit(ev->x, ev->y);
        c->prs = hit;
        if (hit >= 0) {
            c->wp_sel = hit;
            gui_set_wallpaper(kWallpapers[hit]);
            save_settings(c);
            return;
        }
    }
    if (ev->type == EV_KEY_CHAR && c->edit) {
        /* edit username at the cursor: insert/backspace, persist + apply.
         * 39 chars max - fits the 40-byte settings field */
        size_t l = strlen(c->user);
        if (ev->key == 8) {
            if (c->ucur > 0) {
                memmove(c->user + c->ucur - 1, c->user + c->ucur, l - c->ucur + 1);
                c->ucur--;
            }
        } else if (ev->key >= 32 && ev->key < 127 && l < 39) {
            memmove(c->user + c->ucur + 1, c->user + c->ucur, l - c->ucur + 1);
            c->user[c->ucur++] = (char)ev->key;
        } else return;
        gui_set_user_name(c->user);
        save_settings(c);
    }
}
static void set_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Settings", "Settings", 460, 580, -1,
    set_create, set_destroy, set_init, set_draw, set_event, set_update, 0, 0, 0, 0, 1
};
App *make_settings(void) { return &s_app; }
