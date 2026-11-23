/* System volume - the REAL files inside the boot ISO, read straight from
 * the disc image via Limine boot modules (no ISO9660 driver needed).
 * tools/sysvol.sh generates /boot/sysvol.txt (the catalog) and loads it +
 * small text files as modules; this parses them into a browsable tree.
 * Everything here is read-only: it is the OS disk image itself. */
#ifndef SYSVOL_H
#define SYSVOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "vfs.h"   /* reuse VfsEntry shape for name/type/system */

#define SYSVOL_MAX 256
#define SYSVOL_PATH 96

typedef struct {
    VfsEntry v;                 /* name/type(=VFS_DIR|VFS_FILE)/system=1 */
    char     path[SYSVOL_PATH]; /* "boot/kernel" (no leading slash) */
    char     dir[SYSVOL_PATH];  /* parent dir path, "" = volume root */
    uint32_t size;
    int      has_text;          /* content available as a Limine module */
    int      has_blob;          /* binary module (PNG etc.), not text */
} SysVolEnt;

void sysvol_init(void);                          /* parse catalog once */
int  sysvol_ready(void);
int  sysvol_count(const char *dir);              /* children of dir ("" = root) */
const VfsEntry *sysvol_entry(const char *dir, int i);
const SysVolEnt *sysvol_ent(const char *dir, int i); /* full record (untruncated path) */
const SysVolEnt *sysvol_lookup(const char *path);/* full record or NULL */
/* text content of a module-backed file; NULL if binary/unavailable */
int  sysvol_text(const SysVolEnt *e, const char **ptr, uint32_t *len);
/* binary content (PNG etc.) of a module-backed file; NULL if unavailable */
int  sysvol_blob(const SysVolEnt *e, const uint8_t **ptr, uint32_t *len);

#ifdef __cplusplus
}
#endif

#endif
