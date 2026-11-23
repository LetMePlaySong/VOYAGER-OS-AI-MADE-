/* Calculator.app - simple four-function calculator (fixed-point, no FP) */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "../../gui/gui.h"

/* values are stored scaled by 1000 (3 decimal places) */
#define SCALE 1000LL

typedef struct {
    char entry[32];   /* current number being typed */
    char acc[32];     /* stored operand */
    char op;          /* pending operator 0,+,-,*,/ */
    bool just_eval;   /* show result, next digit resets */
    int hov, prs;     /* hovered/pressed button index, -1 none */
    uint64_t prs_t;   /* press start time for click flash */
} CalcCtx;

static const char *kButtons[] = {
    "C",  "+/-", "%",  "/",
    "7",  "8",   "9",  "*",
    "4",  "5",   "6",  "-",
    "1",  "2",   "3",  "+",
    "0",  ".",   "=",  ""
};

#define COLS 4
#define ROWS 5
#define BW 70
#define BH 52
#define GAP 8
#define GRID_X 16
#define GRID_Y 80

/* parse "123.45" -> 123450 (scaled) */
static long long parse_fixed(const char *s) {
    long long ip = 0, fp = 0, fscale = 1;
    bool neg = false, seen_dot = false;
    if (*s == '-') { neg = true; s++; }
    if (*s == '+') s++;
    while (*s) {
        if (*s == '.') { seen_dot = true; s++; continue; }
        if (*s < '0' || *s > '9') break;
        if (!seen_dot) ip = ip * 10 + (*s - '0');
        else if (fscale <= 100) { fp = fp * 10 + (*s - '0'); fscale *= 10; }
        s++;
    }
    while (fscale < 1000) { fp *= 10; fscale *= 10; }
    long long v = ip * SCALE + fp;
    return neg ? -v : v;
}

static long long eval(char op, long long a, long long b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return (a * b) / SCALE;
        case '/': return b != 0 ? (a * SCALE) / b : 0;
    }
    return b;
}

static void show_result(CalcCtx *c, long long v) {
    char tmp[48];
    int ti = 0;
    unsigned long long uv;
    if (v < 0) { tmp[ti++] = '-'; uv = (unsigned long long)(-v); }
    else uv = (unsigned long long)v;
    long long ip = (long long)(uv / SCALE);
    long long fp = (long long)(uv % SCALE);
    char ib[24]; int ii = 0;
    if (ip == 0) ib[ii++] = '0';
    while (ip) { ib[ii++] = (char)('0' + ip % 10); ip /= 10; }
    while (ii) tmp[ti++] = ib[--ii];
    if (fp) {
        tmp[ti++] = '.';
        for (int d = 0; d < 3; d++) {
            fp *= 10;
            int dig = (int)(fp / SCALE);
            tmp[ti++] = (char)('0' + dig);
            fp %= SCALE;
        }
        while (ti > 0 && tmp[ti - 1] == '0') ti--;
        if (ti > 0 && tmp[ti - 1] == '.') ti--;
    }
    tmp[ti] = 0;
    strncpy(c->entry, tmp, sizeof(c->entry) - 1);
    c->entry[sizeof(c->entry) - 1] = 0;
}

static void press(CalcCtx *c, const char *b) {
    char ch = b[0];
    if (!strcmp(b, "C")) {
        c->entry[0] = '0'; c->entry[1] = 0;
        c->acc[0] = 0; c->op = 0; c->just_eval = false;
        return;
    }
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
        if (c->op && !c->just_eval) {
            long long r = eval(c->op, parse_fixed(c->acc), parse_fixed(c->entry));
            show_result(c, r);
            strcpy(c->acc, c->entry);
        } else {
            strcpy(c->acc, c->entry);
        }
        c->op = ch;
        c->just_eval = true;
        strcpy(c->entry, c->acc);
        return;
    }
    if (!strcmp(b, "=")) {
        if (c->op) {
            long long r = eval(c->op, parse_fixed(c->acc), parse_fixed(c->entry));
            show_result(c, r);
            strcpy(c->acc, c->entry);
            c->op = 0;
            c->just_eval = true;
        }
        return;
    }
    if (!strcmp(b, "+/-")) {
        if (c->entry[0] == '-') memmove(c->entry, c->entry + 1, strlen(c->entry));
        else {
            memmove(c->entry + 1, c->entry, strlen(c->entry) + 1);
            c->entry[0] = '-';
        }
        return;
    }
    if (!strcmp(b, "%")) {
        show_result(c, parse_fixed(c->entry) / 100);
        c->just_eval = true;
        return;
    }
    if (ch == '.') {
        if (c->just_eval) { strcpy(c->entry, "0."); c->just_eval = false; return; }
        if (!strchr(c->entry, '.')) {
            size_t l = strlen(c->entry);
            if (l < sizeof(c->entry) - 2) { c->entry[l] = '.'; c->entry[l+1] = 0; }
        }
        return;
    }
    if (c->just_eval) {
        c->entry[0] = ch; c->entry[1] = 0;
        c->just_eval = false;
        return;
    }
    size_t l = strlen(c->entry);
    if (l >= 12) return;
    if (l == 1 && c->entry[0] == '0') {
        c->entry[0] = ch;
    } else {
        c->entry[l] = ch;
        c->entry[l+1] = 0;
    }
}

static void *calc_create(void) {
    CalcCtx *c = (CalcCtx *)kzalloc(sizeof(CalcCtx));
    strcpy(c->entry, "0");
    c->hov = -1; c->prs = -1;
    return c;
}
static void calc_init(void *ctx) { (void)ctx; }
static void calc_destroy(void *ctx) { kfree(ctx); }

static void calc_draw(void *ctx) {
    CalcCtx *c = (CalcCtx *)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(240, 240, 240));
    app_rounded(16, 14, W - 32, 52, 8, rgb(50, 50, 52));
    char tmp[48];
    strncpy(tmp, c->entry, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
    int tw = app_text_width(tmp, 2);
    int tx = W - 28 - tw;
    if (tx < 24) tx = 24;
    app_text(tx, 28, tmp, rgb(255, 255, 255), 2);
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const char *b = kButtons[r * COLS + col];
            if (!b[0]) continue;
            int bx = GRID_X + col * (BW + GAP);
            int by = GRID_Y + r * (BH + GAP);
            uint32_t bg;
            if (b[0] >= '0' && b[0] <= '9') bg = rgb(255, 255, 255);
            else if (!strcmp(b, "=")) bg = rgb(255, 149, 0);
            else bg = rgb(200, 200, 204);
            /* hover = lighter shade of the button's own color,
             * press = darker shade, release = back to normal */
            int bi = r * COLS + col;
            if (c->hov == bi) bg = gui_shade(bg, 22);
            if (c->prs == bi) bg = gui_shade(bg, -38);
            /* '=' spans the last two columns - no empty slot */
            int bw = !strcmp(b, "=") ? 2 * BW + GAP : BW;
            app_rounded(bx, by, bw, BH, 8, bg);
            app_rect(bx, by, bw, BH, rgb(180, 180, 184));
            int lw = app_text_width(b, 2);
            app_text(bx + (bw - lw) / 2, by + (BH - 16) / 2, b, rgb(0,0,0), 2);
        }
    }
}

/* hit-test a button: returns index or -1 (content coords) */
static int calc_hit(int x, int y) {
    for (int r = 0; r < ROWS; r++)
        for (int col = 0; col < COLS; col++) {
            const char *b = kButtons[r * COLS + col];
            if (!b[0]) continue;
            int bx = GRID_X + col * (BW + GAP);
            int by = GRID_Y + r * (BH + GAP);
            int bw = !strcmp(b, "=") ? 2 * BW + GAP : BW;
            if (x >= bx && x < bx + bw && y >= by && y < by + BH)
                return r * COLS + col;
        }
    return -1;
}

static void calc_event(void *ctx, GuiEvent *ev) {
    CalcCtx *c = (CalcCtx *)ctx;
    if (ev->type == EV_MOUSE_MOVE) {
        c->hov = calc_hit(ev->x, ev->y);
        gui_set_cursor(c->hov >= 0 ? CUR_HAND : -1);
        return;
    }
    if (ev->type == EV_MOUSE_UP) { c->prs = -1; return; }
    if (ev->type == EV_MOUSE_DOWN && ev->button == 0) {
        int hit = calc_hit(ev->x, ev->y);
        c->prs = hit;
        if (hit >= 0) { c->prs_t = gui_ticks(); press(c, kButtons[hit]); return; }
    }
    if (ev->type == EV_KEY_CHAR) {
        char buf[2] = {(char)ev->key, 0};
        if ((ev->key >= '0' && ev->key <= '9') || ev->key == '.') press(c, buf);
        else if (ev->key == '+' || ev->key == '-' || ev->key == '*' || ev->key == '/') press(c, buf);
        else if (ev->key == '=') press(c, "=");
        else if (ev->key == 8) { strcpy(c->entry, "0"); c->op = 0; c->just_eval = false; }
    }
}
static void calc_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Calc", "Calculator", 340, 420, -1,
    calc_create, calc_destroy, calc_init, calc_draw, calc_event, calc_update, 0, 0,
    0, 0, 1        /* confirm_close, save_now, no_resize */
};
App *make_calculator(void) { return &s_app; }
