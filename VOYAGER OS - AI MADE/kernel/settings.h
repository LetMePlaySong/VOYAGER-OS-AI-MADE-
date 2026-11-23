/* Persistent OS settings stored in a raw sector on the ATA disk.
 * No filesystem needed: one 512-byte sector at a fixed LBA holds a
 * magic-tagged, checksummed record with wallpaper + username. */
#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* call once at boot; returns 1 if a valid saved record was loaded */
int settings_load(void);
/* write current values; wallpaper/username may be NULL to keep stored */
int settings_save(const char *wallpaper, const char *username);
/* query last-loaded values (NULL if none) */
const char *settings_wallpaper(void);
const char *settings_username(void);
/* desktop icon drag positions: 64 int16 pairs, zeros = default layout */
int  settings_save_icons(const int16_t *dx, const int16_t *dy);
void settings_get_icons(int16_t *dx, int16_t *dy);
/* user-added dock pins, serialized ("A|name\n" / "F|dir|name\n");
 * NULL/"" = none saved. Pinned core apps are NOT stored - they are
 * always present. */
int  settings_save_dock(const char *data);
const char *settings_dock(void);
/* timezone: minutes east of UTC (multiples of 15) + DST rule
 * (0=none, 1=US, 2=EU). settings_set_tz persists immediately. */
void settings_get_tz(int16_t *min, uint8_t *rule, uint8_t *flags);
int  settings_set_tz(int16_t min, uint8_t rule);

#ifdef __cplusplus
}
#endif

#endif
