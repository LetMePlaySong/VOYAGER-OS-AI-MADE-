/* Console.app - disabled stub: the shell isn't finished yet. */
#include <stdint.h>
#include <stdbool.h>
#include "../../gui/gui.h"

static void *term_create(void) { return NULL; }
static void term_destroy(void *ctx) { (void)ctx; }
static void term_init(void *ctx) { (void)ctx; }

static void term_draw(void *ctx) {
    (void)ctx;
    int W = app_content_w(), H = app_content_h();
    app_fill(0, 0, W, H, rgb(24, 26, 30));
    const char *msg = "Console doesn't work yet!";
    int tw = app_text_width(msg, 2);
    app_text((W - tw) / 2, H / 2 - 10, msg, rgb(220, 220, 230), 2);
    const char *sub = "This app is disabled for now.";
    int sw = app_text_width(sub, 1);
    app_text((W - sw) / 2, H / 2 + 14, sub, rgb(140, 140, 155), 1);
}

static void term_event(void *ctx, GuiEvent *ev) { (void)ctx; (void)ev; }
static void term_update(void *ctx) { (void)ctx; }

static App s_app = {
    "Console", "Console", 480, 300, -1,
    term_create, term_destroy, term_init, term_draw, term_event, term_update, 0, 0
};
App *make_console(void) { return &s_app; }
