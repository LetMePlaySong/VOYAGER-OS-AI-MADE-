/* GUI: window manager, desktop, menu bar, dock - Squarebin style */
#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- colors (0xRRGGBB) ---- */
#define COL_WHITE      0xFFFFFF
#define COL_BLACK      0x000000
#define COL_DARK       0x1E1E1E
#define COL_GRAY       0x888888
#define COL_LIGHTGRAY  0xDDDDDD
#define COL_MENU_BG    0xF6F6F6
#define COL_WIN_BG     0xECECEC
#define COL_TITLEBAR   0xD8D8D8
#define COL_TITLE_ACTIVE 0xE8E8E8
#define COL_BORDER     0xA0A0A0
#define COL_BLUE       0x6B4FE0   /* Voyager indigo accent (was macOS blue) */
#define COL_GREEN      0x34C759
#define COL_RED        0xFF3B30
#define COL_YELLOW     0xFFCC00
#define COL_ORANGE     0xFF9500
#define COL_TEXT       0x222222
#define COL_TEXT_LIGHT 0x666666
#define COL_SELECTION  0x6B4FE0

/* ---- events ---- */
enum {
    EV_NONE = 0,
    EV_MOUSE_MOVE,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_WHEEL,
    EV_KEY_CHAR,     /* printable char in `key` */
    EV_KEY_SPECIAL,  /* arrows etc: key = code */
    EV_TICK          /* periodic update */
};

/* special key codes (>=0x80 in kbd buffer) */
#define KEY_UP      0x81
#define KEY_DOWN    0x82
#define KEY_LEFT    0x83
#define KEY_RIGHT   0x84
#define KEY_HOME    0x85
#define KEY_END     0x86
#define KEY_ESC     0x1B
#define KEY_BACKSPACE 8
#define KEY_ENTER   10
#define KEY_TAB     9
#define KEY_DELETE  0x87

typedef struct {
    int type;
    int x, y;          /* window-content-relative for app events */
    int gx, gy;        /* global framebuffer coords */
    int button;        /* 0=left 1=right 2=middle */
    int key;           /* char or special code */
    int wheel;         /* wheel delta for EV_MOUSE_WHEEL */
} GuiEvent;

/* ---- app interface ---- */
typedef struct App App;
struct App {
    const char *name;      /* internal name e.g. "Voyage Web" */
    const char *title;     /* window title */
    int width, height;     /* default window size */
    int icon_id;           /* asset index for icon, -1 = none */
    void *(*create)(void);
    void (*destroy)(void *ctx);
    void (*init)(void *ctx);
    void (*draw)(void *ctx);              /* draw inside content rect */
    void (*event)(void *ctx, GuiEvent *ev);
    void (*update)(void *ctx);            /* called every frame */
    /* content size queries */
    int content_w, content_h;
    /* optional: return nonzero to veto window close (e.g. unsaved changes);
     * save_now is invoked by the "Do you want to save?" dialog on Save */
    int  (*confirm_close)(void *ctx);
    void (*save_now)(void *ctx);
    int no_resize;          /* 1 = fixed-size window: no grip, no zoom */
};

/* ---- public API ---- */
void gui_init(void);
void gui_early_splash(void);   /* loading screen from the first framebuffer frame */
void gui_main_loop(void);
void gui_boot_chime(void);                       /* Voyager startup "DA BING" */
void gui_alert_beep(void);                       /* short deny/error beep */
void gui_alert(int kind, const char *title, const char *msg);
                                             /* 0=error 1=warning 2=info */

void gui_register_app(App *app);
int  gui_app_count(void);
App *gui_get_app(int i);
App *gui_find_app(const char *name);

void gui_open_app(App *app);
void gui_open_app_new(App *app);   /* always spawn a new window instance */
void gui_open_app_named(const char *name);
const char *gui_pending_file(void);      /* VFS file Quill should open, NULL */
void gui_set_pending_file(const char *name);
void gui_clear_pending_file(void);
const char *gui_pending_dir(void);       /* folder the pending file lives in */
void gui_set_pending_path(const char *dir, const char *name);
const char *gui_pending_folder(void);    /* VFS folder Files should show */
void gui_set_pending_folder(const char *name);
void gui_clear_pending_folder(void);
const char *gui_pending_sysfile(void);   /* ISO system-volume path Quill opens */
void gui_set_pending_sysfile(const char *path);
void gui_clear_pending_sysfile(void);
const char *gui_pending_image(void);     /* "dir/name" PNG Photos should open */
void gui_set_pending_image(const char *dir, const char *name);
void gui_clear_pending_image(void);
const char *gui_pending_audio(void);     /* track path Voyage Player plays */
void gui_set_pending_audio(const char *path);
void gui_clear_pending_audio(void);
int  gui_is_audio(const char *name);     /* .mp3/.wav/.ogg case-insensitive */
void gui_clear_pending_folder(void);
void gui_request_rename(const char *dir, const char *name); /* "Pick a name" */
void gui_rtc_time(char *buf, int size);   /* "DD/MM/YY HH:MM:SS AM" */
void gui_rtc_local_hms(int *h, int *m, int *s);  /* local civil time, 24h */
void gui_rtc_set_local(int h, int m, int s);     /* set clock (writes RTC) */
int  gui_time_synced(void);               /* SNTP/manual set gave trusted time */
void gui_sntp_sync_now(void);             /* force an immediate time sync */
int  gui_time_sync_state(void);           /* 0 idle 1 syncing 2 ok 3 failed */
bool gui_app_is_open(App *app);
void gui_close_window(int win_id);
void gui_close_current_window(void);   /* close the window handling this event */
void gui_focus_window(int win_id);

/* drawing helpers for apps (content-relative coords) */
void app_fill(int x, int y, int w, int h, uint32_t color);
void app_blend(int x, int y, int w, int h, uint32_t color);
void app_rect(int x, int y, int w, int h, uint32_t color);
void app_rounded(int x, int y, int w, int h, int rad, uint32_t color);
void app_text(int x, int y, const char *s, uint32_t color, int scale);
int  app_text_clip(int x, int y, const char *s, uint32_t color, int scale,
                   int cx, int cy, int cw, int ch);
int  app_text_width(const char *s, int scale);
/* fractional-height text: h = on-screen glyph height in px (zoom) */
void app_text_q(int x, int y, const char *s, uint32_t color, int h);
int  app_text_clip_q(int x, int y, const char *s, uint32_t color, int h,
                     int cx, int cy, int cw, int ch);
/* truncate `src` with a "..." suffix so it fits `max_w` pixels (scale 1) */
void gui_fit_label(char *dst, size_t cap, const char *src, int max_w);
void app_draw_image(int x, int y, const uint8_t *rgba, int w, int h,
                    int sw, int sh);
void app_draw_image_scaled(int x, int y, int w, int h,
                           const uint8_t *rgba, int sw, int sh);

/* content area of currently-drawing window (set during draw/event) */
int  app_content_w(void);
int  app_content_h(void);
int  app_content_x(void); /* global x of content origin */
int  app_content_y(void);
int  gui_win_focused(void);   /* nonzero while this app's window has focus */
/* task-manager support: enumerate live windows */
int  gui_win_list_count(void);
/* fills app name + window title for row i; returns win id, -1 oob.
 * focused/minimized flags via out params (may be NULL) */
int  gui_win_list_get(int i, char *app, int acap, char *title, int tcap,
                      int *focused, int *minimized);

/* assets */
const uint8_t *asset_png(const char *name, size_t *size);
const uint8_t *asset_image(const char *name, uint32_t *w, uint32_t *h); /* decoded RGBA, cached */
const uint8_t *gui_vfs_image(const char *dir, const char *name,
                             uint32_t *w, uint32_t *h);
/* small cached thumbnails for icons (<=96px) - cheap to call every frame */
const uint8_t *gui_vfs_thumb(const char *dir, const char *name,
                             uint32_t *w, uint32_t *h);
const uint8_t *gui_sysvol_thumb(const char *path,
                                uint32_t *w, uint32_t *h);   /* VFS PNG -> RGBA */
/* decoded RGBA for an ISO system-volume PNG (read-only, cached) */
const uint8_t *gui_sysvol_image(const char *path,
                                uint32_t *w, uint32_t *h);
int asset_index(const char *name);
void asset_draw(int x, int y, int size, const char *name); /* draw icon centered-ish */
const char *gui_file_icon(const char *name);   /* .txt -> quill, else "?" */
int  gui_is_png(const char *name);             /* case-insensitive .png */
void gui_notify(const char *msg);              /* transient banner message */
/* "plop" drop animation: icon arcs from (x0,y0) into (x1,y1) while
 * shrinking + an expanding ring. `icon` is an asset name; if `name`
 * (in VFS dir `dir`) is a decodable .png its pixels fly instead. */
void gui_plop(int x0, int y0, int x1, int y1,
              const char *icon, const char *dir, const char *name);
/* reverse plop: item slides OUT of (x0,y0) to the cursor at (x1,y1) */
void gui_plop_out(int x0, int y0, int x1, int y1,
                  const char *icon, const char *dir, const char *name);
/* app dragged out of a window onto the desktop -> ".lnk" pinned at (x,y) */
void gui_drop_app_shortcut(const char *appname, int x, int y);
/* pin a desktop icon's center at screen point (x,y) */
void gui_place_icon_at(const char *name, int x, int y);
/* ISO system-volume PNG path Photos should open read-only */
void gui_set_pending_sysimg(const char *path);
const char *gui_pending_sysimg(void);
void gui_clear_pending_sysimg(void);
/* dock pinning: add an app (by name) or a VFS item (dir "" = root) */
int  gui_dock_add_app(const char *appname);
int  gui_dock_add_vfs(const char *dir, const char *name);

/* misc */
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t gui_shade(uint32_t c, int delta);
void gui_request_redraw(void);
uint64_t gui_ticks(void);
void gui_sleep_ms(int ms);
const char *gui_user_name(void);
void gui_set_user_name(const char *name);
const char *gui_wallpaper_name(void);
void gui_set_wallpaper(const char *name);

/* ---- cursors ---- */
enum {
    CUR_ARROW = 0, CUR_IBEAM, CUR_HAND, CUR_HAND_OPEN, CUR_HAND_GRAB,
    CUR_BUSY, CUR_CROSS, CUR_MOVE, CUR_HELP,
    CUR_RESIZE_N, CUR_RESIZE_S, CUR_RESIZE_E, CUR_RESIZE_W,
    CUR_RESIZE_NE, CUR_RESIZE_NW, CUR_RESIZE_SE, CUR_RESIZE_SW,
    CUR_RESIZE_LEFTRIGHT, CUR_RESIZE_UPDOWN,
    CUR_ZOOMIN, CUR_ZOOMOUT, CUR_CELL, CUR_CONTEXTMENU,
    CUR_NOTALLOWED, CUR_COPY, CUR_COUNT
};
/* request a cursor for the pointer; pass -1 for automatic hover detection */
void gui_set_cursor(int type);

#ifdef __cplusplus
}
#endif

#endif
