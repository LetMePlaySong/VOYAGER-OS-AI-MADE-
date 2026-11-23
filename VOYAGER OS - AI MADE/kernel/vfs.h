/* Minimal persistent filesystem - no partitions, just raw ATA sectors.
 * Sector 200 = directory block, sectors 201+ = one 512-byte content sector
 * per file. Folders nest via a parent slot index (0xFF = root).
 * Everything is cached in RAM; writes go straight to the disk. */
#ifndef VFS_H
#define VFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_FILES 48
#define VFS_NAME_LEN  24
#define VFS_CONTENT   500          /* usable bytes per file (sector minus hdr) */

enum { VFS_DIR, VFS_FILE };

typedef struct {
    char     name[VFS_NAME_LEN + 1];
    uint8_t  type;               /* VFS_DIR or VFS_FILE */
    int      parent;             /* live index of parent folder, -1 = root */
    uint8_t  used;
    uint8_t  system;             /* 1 = protected system entry (no modify) */
} VfsEntry;

void vfs_init(void);                          /* scan disk -> RAM dir */
int  vfs_count(void);                         /* live entries at ROOT only */
int  vfs_count_in(const char *folder);        /* children of folder (NULL=root) */
const VfsEntry *vfs_entry(int i);             /* i-th ROOT entry, NULL if oob */
const VfsEntry *vfs_entry_in(const char *folder, int i);
int  vfs_find(const char *name);              /* live index at root or -1 */
int  vfs_slot_root(const char *name);         /* stable slot index or -1 */
int  vfs_find_in(const char *folder, const char *name);
int  vfs_create(const char *name, int type);  /* at root: 0 ok, -1 exists/full */
int  vfs_create_in(const char *name, int type, const char *parent);
int  vfs_rename(const char *oldname, const char *newname);
int  vfs_rename_in(const char *folder, const char *oldname, const char *newname);
int  vfs_delete(const char *name);            /* root scope, recursive */
int  vfs_delete_in(const char *folder, const char *name);
int  vfs_move(const char *name, const char *src_folder, const char *dst_folder);
int  vfs_parent_name(const char *folder, char *out, int cap); /* "" = root */
int  vfs_read(const char *name, char *buf, int cap);    /* returns len or -1 */
int  vfs_write(const char *name, const char *buf, int len); /* 0 ok */
int  vfs_read_in(const char *folder, const char *name, char *buf, int cap);
int  vfs_write_in(const char *folder, const char *name, const char *buf, int len);
int  vfs_read_raw_in(const char *folder, const char *name, uint8_t *out); /* 512B sector */
int  vfs_file_size_in(const char *folder, const char *name);  /* bytes, -1 none */
/* read ALL content sectors of a binary file; returns stored byte length */
int  vfs_read_raw_full_in(const char *folder, const char *name,
                          uint8_t *out, int cap);
/* largest contiguous free content run in sectors (0 = disk full);
 * a new file of N bytes needs (N+511)/512 <= this to be saved */
int  vfs_free_run_sectors(void);

/* foreign Voyager volumes on other attached disks (USB/second VDI).
 * midx matches ata_media_info ordering. dirslot = directory slot
 * index (-1 = volume root). */
int  vfsx_probe(int midx);                       /* 1 = Voyager volume */
int  vfsx_count(int midx, int dirslot);
int  vfsx_get(int midx, int dirslot, int i,
              VfsEntry *out, int *slot);
int  vfsx_parent(int midx, int dirslot);         /* -1 root, -2 invalid */
int  vfsx_read(int midx, int slot, uint8_t *out, int cap);
int  vfsx_write(int midx, int dirslot, const char *name,
                const char *buf, int len);
int  vfsx_size(int midx, int slot);            /* file bytes, 0 = none */

#ifdef __cplusplus
}
#endif

#endif
