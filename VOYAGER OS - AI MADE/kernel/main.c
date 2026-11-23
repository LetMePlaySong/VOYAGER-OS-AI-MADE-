/* Kernel entry point - called by Limine in 64-bit mode, higher half,
 * with a bootloader-provided stack (stack size request). */
#include "k.h"
#include "arch/arch.h"
#include "fb.h"
#include "../net/net.h"
#include "../gui/gui.h"
#include "settings.h"
#include "vfs.h"
#include "../drivers/ac97.h"

/* app factories (C++ apps) */
App *app_make_finder(void);
App *app_make_voyageweb(void);
App *app_make_quill(void);
App *app_make_calculator(void);
App *app_make_console(void);
App *app_make_settings(void);
App *app_make_memo(void);
App *app_make_photos(void);
App *app_make_voyageplayer(void);

static void register_apps(void) {
    gui_register_app(app_make_finder());
    gui_register_app(app_make_voyageweb());
    gui_register_app(app_make_quill());
    gui_register_app(app_make_calculator());
    gui_register_app(app_make_console());
    gui_register_app(app_make_settings());
    gui_register_app(app_make_memo());
    gui_register_app(app_make_voyageplayer());
    /* Photos/Preview is NOT registered: it has no icon, dock entry or
     * Applications-grid presence. .png double-clicks spawn it directly
     * via app_make_photos() + gui_open_app_new(). */
}

void kernel_main(void) {
    __asm__ volatile("cli");   /* no IRQs until our IDT+PIC are ready */
    __asm__ volatile("clts");  /* clear CR0.TS so SSE can't #NM */
    /* enable SSE/SSE2: minimp3 (MP3 decode) uses float/SSE. Without
     * CR4.OSFXSR+OSXMMEXCPT and CR0.EM=0/MP=1 every SSE insn is #UD. */
    __asm__ volatile(
        "movq %%cr0, %%rax\n\t"
        "andq $-5, %%rax\n\t"            /* CR0.EM = 0 (bit2) */
        "orq  $0x2, %%rax\n\t"         /* CR0.MP = 1  (bit1) */
        "movq %%rax, %%cr0\n\t"
        "movq %%cr4, %%rax\n\t"
        "orq  $0x600, %%rax\n\t"        /* OSFXSR | OSXMMEXCPT */
        "movq %%rax, %%cr4\n\t"
        ::: "rax");
    __asm__ volatile("ldmxcsr %0" :: "m"(*(const uint32_t[]){0x1f80}));
    __asm__ volatile("fninit");
    uart_init();
    kprint("\n=== Voyager OS ===\n");

    /* early framebuffer: paint a neutral desktop backdrop so the
     * screen doesn't sit on the bootloader's last frame or a debug
     * color while drivers init. GUI wallpaper covers this later. */
    fb_init();
    if (fb_ready()) {
        fb_fill_rect(0, 0, fb_width(), fb_height(), fb_rgb(10, 10, 40));
        fb_present();
    }

    arch_init();
    kprint("arch: gdt/idt/pit/ps2 ok\n");
    heap_selftest();

    /* network: non-fatal if no e1000 */
#ifndef SKIP_NET
    if (net_init() == 0) {
        kprint("net: online\n");
    }
#endif

    /* restore persisted settings (wallpaper, username) before GUI builds */
    if (settings_load()) {
        const char *wp = settings_wallpaper();
        const char *un = settings_username();
        kprintf("settings: restored wp=%s user=%s\n", wp ? wp : "-", un ? un : "-");
        if (wp && wp[0]) gui_set_wallpaper(wp);
        if (un && un[0]) gui_set_user_name(un);
    }

    /* filesystem: scan the raw-sector store before the desktop builds */
    vfs_init();

    gui_init();
    gui_early_splash();   /* Voyager loading screen covers ALL of init */
    kprint("gui: ready\n");

    register_apps();
    kprint("apps registered\n");

    /* audio: non-fatal if no AC97 device */
    ac97_init();

    gui_boot_chime();   /* Voyager "DA BING" startup sound (real speakers) */

    gui_main_loop(); /* never returns */
}

void _start(void) {
    kernel_main();
    for (;;) __asm__ volatile("hlt");
}
