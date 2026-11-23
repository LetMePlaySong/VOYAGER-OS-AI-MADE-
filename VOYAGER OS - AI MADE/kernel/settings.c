/* Persistent settings sector - see settings.h */
#include <stdint.h>
#include <string.h>
#include "settings.h"
#include "k.h"
#include "../drivers/ata.h"

#define SETTINGS_LBA   100          /* safely past any partition tables */
#define SETTINGS_MAGIC 0x31514253u  /* "SQB1" */
#define SETTINGS_VER   4

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    char     wallpaper[40];
    char     username[40];
    int16_t  icon_dx[64];           /* desktop icon drag offsets */
    int16_t  icon_dy[64];
    char     dock[160];             /* serialized user dock pins */
    int16_t  tz_min;                /* timezone offset, minutes east of UTC
                                     * (e.g. -300 = UTC-5 Eastern) */
    uint8_t  dst_rule;              /* 0=none, 1=US, 2=EU daylight-saving */
    uint8_t  tz_flags;
    uint32_t checksum;             /* sum of all preceding bytes */
} SettingsSector;

static char s_wallpaper[40];
static char s_username[40];
static int16_t s_icon_dx[64], s_icon_dy[64];
static char s_dock[160];
static int16_t s_tz_min;
static uint8_t s_dst_rule, s_tz_flags;
static int  s_loaded;
static int  s_icons_loaded;

static uint32_t sector_checksum(const SettingsSector *s) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t sum = 0;
    for (size_t i = 0; i < offsetof(SettingsSector, checksum); i++)
        sum = sum * 31 + p[i];
    return sum;
}

static void sanitize(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] < 0x20 || dst[i] > 0x7E) dst[i] = ' ';
}

int settings_load(void) {
    if (!ata_present()) return 0;
    SettingsSector s;
    if (ata_read_sector(SETTINGS_LBA, &s) != 0) return 0;
    if (s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VER) return 0;
    if (sector_checksum(&s) != s.checksum) return 0;
    sanitize(s_wallpaper, sizeof(s_wallpaper), s.wallpaper);
    sanitize(s_username, sizeof(s_username), s.username);
    memcpy(s_icon_dx, s.icon_dx, sizeof(s_icon_dx));
    memcpy(s_icon_dy, s.icon_dy, sizeof(s_icon_dy));
    memcpy(s_dock, s.dock, sizeof(s.dock));
    s_dock[sizeof(s_dock) - 1] = 0;
    s_tz_min = s.tz_min;
    s_dst_rule = s.dst_rule;
    s_tz_flags = s.tz_flags;
    s_icons_loaded = 1;
    s_loaded = 1;
    return 1;
}

int settings_save(const char *wallpaper, const char *username) {
    if (!ata_present()) return -1;
    SettingsSector s;
    memset(&s, 0, sizeof(s));
    s.magic = SETTINGS_MAGIC;
    s.version = SETTINGS_VER;
    sanitize(s.wallpaper, sizeof(s_wallpaper),
             wallpaper ? wallpaper : (s_loaded ? s_wallpaper : ""));
    sanitize(s.username, sizeof(s_username),
             username ? username : (s_loaded ? s_username : ""));
    /* mirror sanitized values into the runtime cache (fixes stale-cache
     * overwrite that made new selections never reach the disk) */
    strncpy(s_wallpaper, s.wallpaper, sizeof(s_wallpaper) - 1);
    s_wallpaper[sizeof(s_wallpaper) - 1] = 0;
    strncpy(s_username, s.username, sizeof(s.username) - 1);
    s_username[sizeof(s_username) - 1] = 0;
    memcpy(s.icon_dx, s_icons_loaded ? s_icon_dx : (int16_t[64]){0},
           sizeof(s.icon_dx));
    memcpy(s.icon_dy, s_icons_loaded ? s_icon_dy : (int16_t[64]){0},
           sizeof(s.icon_dy));
    memcpy(s.dock, s_dock, sizeof(s.dock));
    s.tz_min = s_tz_min; s.dst_rule = s_dst_rule; s.tz_flags = s_tz_flags;
    s.checksum = sector_checksum(&s);
    if (ata_write_sector(SETTINGS_LBA, &s) != 0) return -1;
    s_loaded = 1;
    return 0;
}

const char *settings_wallpaper(void) { return s_loaded ? s_wallpaper : NULL; }
const char *settings_username(void)  { return s_loaded ? s_username : NULL; }

/* write ONLY the icon-position arrays, preserving wallpaper/username */
int settings_save_icons(const int16_t *dx, const int16_t *dy) {
    if (!ata_present() || !dx || !dy) return -1;
    SettingsSector s;
    memset(&s, 0, sizeof(s));
    s.magic = SETTINGS_MAGIC;
    s.version = SETTINGS_VER;
    sanitize(s.wallpaper, sizeof(s.wallpaper),
             s_loaded ? s_wallpaper : "");
    sanitize(s.username, sizeof(s.username),
             s_loaded ? s_username : "");
    memcpy(s.icon_dx, dx, sizeof(s.icon_dx));
    memcpy(s.icon_dy, dy, sizeof(s.icon_dy));
    memcpy(s.dock, s_dock, sizeof(s.dock));
    memcpy(s_icon_dx, dx, sizeof(s_icon_dx));
    memcpy(s_icon_dy, dy, sizeof(s_icon_dy));
    s_icons_loaded = 1;
    s.tz_min = s_tz_min; s.dst_rule = s_dst_rule; s.tz_flags = s_tz_flags;
    s.checksum = sector_checksum(&s);
    if (ata_write_sector(SETTINGS_LBA, &s) != 0) return -1;
    s_loaded = 1;
    return 0;
}

void settings_get_icons(int16_t *dx, int16_t *dy) {
    if (dx) memcpy(dx, s_icons_loaded ? s_icon_dx : (int16_t[64]){0},
                   sizeof(s_icon_dx[0]) * 64);
    if (dy) memcpy(dy, s_icons_loaded ? s_icon_dy : (int16_t[64]){0},
                   sizeof(s_icon_dy[0]) * 64);
}

/* dock pins: rewrite ONLY the dock string, preserving everything else */
int settings_save_dock(const char *data) {
    if (!ata_present()) return -1;
    SettingsSector s;
    memset(&s, 0, sizeof(s));
    s.magic = SETTINGS_MAGIC;
    s.version = SETTINGS_VER;
    sanitize(s.wallpaper, sizeof(s.wallpaper),
             s_loaded ? s_wallpaper : "");
    sanitize(s.username, sizeof(s.username),
             s_loaded ? s_username : "");
    memcpy(s.icon_dx, s_icons_loaded ? s_icon_dx : (int16_t[64]){0},
           sizeof(s.icon_dx));
    memcpy(s.icon_dy, s_icons_loaded ? s_icon_dy : (int16_t[64]){0},
           sizeof(s.icon_dy));
    memset(s.dock, 0, sizeof(s.dock));
    if (data) {
        strncpy(s.dock, data, sizeof(s.dock) - 1);
        s.dock[sizeof(s.dock) - 1] = 0;
        for (int i = 0; s.dock[i]; i++)
            if (s.dock[i] < 0x20 && s.dock[i] != '\n') s.dock[i] = ' ';
    }
    memcpy(s_dock, s.dock, sizeof(s_dock));
    s_dock[sizeof(s_dock) - 1] = 0;
    s.tz_min = s_tz_min; s.dst_rule = s_dst_rule; s.tz_flags = s_tz_flags;
    s.checksum = sector_checksum(&s);
    if (ata_write_sector(SETTINGS_LBA, &s) != 0) return -1;
    s_loaded = 1;
    return 0;
}

const char *settings_dock(void) { return s_dock[0] ? s_dock : NULL; }

/* timezone: minutes east of UTC + DST rule (0 none, 1 US, 2 EU).
 * settings_set_tz rewrites only those bytes, preserving everything else. */
void settings_get_tz(int16_t *min, uint8_t *rule, uint8_t *flags) {
    if (min) *min = s_loaded ? s_tz_min : 0;
    if (rule) *rule = s_loaded ? s_dst_rule : 0;
    if (flags) *flags = s_loaded ? s_tz_flags : 0;
}
int settings_set_tz(int16_t min, uint8_t rule) {
    if (!ata_present()) return -1;
    if (min < -720 || min > 840 || min % 15 != 0) return -1;
    if (rule > 2) return -1;
    SettingsSector s;
    memset(&s, 0, sizeof(s));
    s.magic = SETTINGS_MAGIC;
    s.version = SETTINGS_VER;
    sanitize(s.wallpaper, sizeof(s.wallpaper), s_loaded ? s_wallpaper : "");
    sanitize(s.username, sizeof(s.username), s_loaded ? s_username : "");
    memcpy(s.icon_dx, s_icons_loaded ? s_icon_dx : (int16_t[64]){0},
           sizeof(s.icon_dx));
    memcpy(s.icon_dy, s_icons_loaded ? s_icon_dy : (int16_t[64]){0},
           sizeof(s.icon_dy));
    memset(s.dock, 0, sizeof(s.dock));
    if (s_dock[0]) {
        strncpy(s.dock, s_dock, sizeof(s.dock) - 1);
        s.dock[sizeof(s.dock) - 1] = 0;
    }
    s.tz_min = min; s.dst_rule = rule; s.tz_flags = 1;
    s_tz_min = min; s_dst_rule = rule; s_tz_flags = 1;
    s.checksum = sector_checksum(&s);
    if (ata_write_sector(SETTINGS_LBA, &s) != 0) return -1;
    s_loaded = 1;
    return 0;
}
