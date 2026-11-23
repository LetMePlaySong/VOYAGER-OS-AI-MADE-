/* Bridge: C++ app factories -> C linkage for kernel main.c */
#include "gui.h"

App *make_finder(void);
App *make_voyageweb(void);
App *make_quill(void);
App *make_calculator(void);
App *make_console(void);
App *make_settings(void);
App *make_memo(void);
App *make_photos(void);
App *make_voyageplayer(void);

extern "C" {
App *app_make_finder(void)      { return make_finder(); }
App *app_make_voyageweb(void)      { return make_voyageweb(); }
App *app_make_quill(void)    { return make_quill(); }
App *app_make_calculator(void)  { return make_calculator(); }
App *app_make_console(void)    { return make_console(); }
App *app_make_settings(void)    { return make_settings(); }
App *app_make_memo(void)       { return make_memo(); }
App *app_make_photos(void)      { return make_photos(); }
App *app_make_voyageplayer(void)      { return make_voyageplayer(); }
}
