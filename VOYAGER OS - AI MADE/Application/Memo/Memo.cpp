/* Memo.app - yellow sticky notes */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "../../gui/gui.h"

#define NOTE_MAX 2048
#define NOTE_LINES 18
#define NOTE_LINE_W 40

typedef struct {
    char text[NOTE_MAX];
    int len;
    int cursor;
    bool editing;   /* caret only after clicking into the note */
} NoteCtx;

static void *note_create(void) {
    NoteCtx *c = (NoteCtx *)kzalloc(sizeof(NoteCtx));
    strcpy(c->text, "Memo\n\nDouble-click to edit.\n");
    c->len = strlen(c->text);
    c->cursor = c->len;
    return c;
}
static void note_init(void *ctx) { (void)ctx; }
static void note_destroy(void *ctx) { kfree(ctx); }

static void note_insert(NoteCtx *c, char ch) {
    if (c->len >= NOTE_MAX - 1) return;
    if (c->cursor < c->len)
        memmove(c->text + c->cursor + 1, c->text + c->cursor, c->len - c->cursor);
    c->text[c->cursor++] = ch;
    c->len++;
    c->text[c->len] = 0;
}

static void note_draw(void *ctx) {
    NoteCtx *c = (NoteCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    /* sticky note yellow */
    app_fill(0, 0, W, H, rgb(255, 244, 160));
    app_rect(0, 0, W, H, rgb(230, 210, 90));
    /* text lines */
    int y = 14;
    const char *p = c->text;
    int line = 0;
    while (*p && line < NOTE_LINES) {
        const char *ls = p;
        int lw = 0;
        while (*p && *p != '\n') { lw++; p++; }
        if (*p == '\n') p++;
        char tmp[NOTE_LINE_W + 4];
        int n = lw < NOTE_LINE_W ? lw : NOTE_LINE_W;
        memcpy(tmp, ls, n); tmp[n] = 0;
        app_text_clip(14, y, tmp, rgb(60, 50, 10), 1, 0, 0, W, H);
        y += 11;
        line++;
    }
    /* blinking caret while editing and focused */
    if (c->editing && gui_win_focused() && ((gui_ticks() / 500) & 1) == 0) {
        int cl = 0, cc = 0;
        for (int i = 0; i < c->cursor && c->text[i]; i++) {
            if (c->text[i] == '\n') { cl++; cc = 0; } else cc++;
        }
        if (cl < NOTE_LINES)
            app_fill(14 + cc * 9, 14 + cl * 11, 2, 9, rgb(60, 50, 10));
    }
}

/* map click position to byte offset; outside content = stop editing */
static void note_click_pos(NoteCtx *c, int x, int y) {
    int W = app_content_w(), H = app_content_h();
    if (x < 0 || y < 0 || x >= W || y >= H) { c->editing = false; return; }
    c->editing = true;
    int line = (y - 14) / 11;
    int col = (x - 14) / 9;
    if (line < 0) line = 0;
    if (col < 0) col = 0;
    int cl = 0, cc = 0, i = 0;
    while (i < c->len) {
        if (cl == line && cc >= col) break;
        if (c->text[i] == '\n') { cl++; cc = 0; } else cc++;
        i++;
    }
    c->cursor = i;
}

/* byte offset -> {line, col}; {line, col} -> byte offset (clamped) */
static void note_pos(const NoteCtx *c, int *line, int *col) {
    *line = 0; *col = 0;
    for (int i = 0; i < c->cursor && i < c->len; i++) {
        if (c->text[i] == '\n') { (*line)++; *col = 0; } else (*col)++;
    }
}
static int note_off(const NoteCtx *c, int line, int col) {
    int l = 0, cc = 0, i = 0;
    while (i < c->len && l < line) {
        if (c->text[i] == '\n') l++; i++;
    }
    while (i < c->len && c->text[i] != '\n' && cc < col) { cc++; i++; }
    return i;
}
static void note_key(NoteCtx *c, int key) {
    int line, col;
    if (key == KEY_LEFT) { if (c->cursor > 0) c->cursor--; return; }
    if (key == KEY_RIGHT) { if (c->cursor < c->len) c->cursor++; return; }
    if (key == KEY_HOME) {
        note_pos(c, &line, &col); c->cursor = note_off(c, line, 0); return;
    }
    if (key == KEY_END) {
        note_pos(c, &line, &col);
        int i = c->cursor;
        while (i < c->len && c->text[i] != '\n') i++;
        c->cursor = i; return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        note_pos(c, &line, &col);
        int nl = line + (key == KEY_DOWN ? 1 : -1);
        if (nl < 0) nl = 0;
        /* clamp to last line */
        int maxl = 0;
        for (int i = 0; i < c->len; i++) if (c->text[i] == '\n') maxl++;
        if (nl > maxl) nl = maxl;
        c->cursor = note_off(c, nl, col);
    }
}

static void note_event(void *ctx, GuiEvent *ev) {
    NoteCtx *c = (NoteCtx *)ctx;
    if (ev->type == EV_MOUSE_MOVE) {
        gui_set_cursor(c->editing ? CUR_IBEAM : -1);
        return;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        note_click_pos(c, ev->x, ev->y);
        return;
    }
    if (!c->editing) return;
    if (ev->type == EV_KEY_SPECIAL) { note_key(c, ev->key); return; }
    if (ev->type == EV_KEY_CHAR) {
        if (ev->key == 8) {
            if (c->cursor > 0) {
                memmove(c->text + c->cursor - 1, c->text + c->cursor, c->len - c->cursor);
                c->cursor--; c->len--; c->text[c->len] = 0;
            }
            return;
        }
        if (ev->key == 10) { note_insert(c, '\n'); return; }
        if (ev->key >= 32 && ev->key < 127) note_insert(c, (char)ev->key);
    }
}
static void note_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Memo", "Memo", 320, 380, -1,
    note_create, note_destroy, note_init, note_draw, note_event, note_update, 0, 0
};
App *make_memo(void) { return &s_app; }
