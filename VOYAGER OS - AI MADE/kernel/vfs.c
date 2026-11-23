/* Minimal persistent filesystem - see vfs.h.
 * LBA 200 = directory sector, LBAs 201..216 = file content sectors.
 * Folders nest: each entry stores its parent folder's slot index
 * (0xFF = desktop root). */
#include <stdint.h>
#include <string.h>
#include "vfs.h"
#include "k.h"
#include "../drivers/ata.h"

#define DIR_LBA      200
#define DIR_SECTORS  4              /* directory spans LBAs 200-203 (2 KiB) */
#define CONTENT_BASE 204            /* file content sectors start after dir */
#define MAX_SECTORS  2048           /* ~1 MiB content pool on the VDI */
#define DIR_MAGIC    0x31564656u    /* legacy single-sector directory */
#define DIR_MAGIC2   0x32564656u    /* current multi-sector directory */
#define DIR_VER_SIZE 3             /* version with per-file size metadata */
#define PARENT_ROOT  0xFF

/* per-file size metadata lives in DirSector.tail (600 spare bytes):
 * tail[128..224) = uint16 sector-count per slot (0 = 1, legacy files)
 * tail[224..320) = uint16 byte-length per slot (0 = whole sectors) */
#define TAIL_SECS_OFF   128
#define TAIL_BYTES_OFF  224

#define VF_SYSTEM 0x01   /* protected: cannot delete/rename/move/contain */

typedef struct __attribute__((packed)) {
    char    name[VFS_NAME_LEN + 1];
    uint8_t type;
    uint8_t parent;              /* parent slot index, PARENT_ROOT = desktop */
    uint16_t lba;                /* content sector, 0 = none (folder) */
} DirEnt;                          /* 29 bytes - UNCHANGED layout so old
                                     disks stay readable */

/* legacy 512-byte directory (VFS_MAX_FILES was 16) - kept for migration */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
    uint16_t pad;
    DirEnt   ents[16];
    uint8_t  flags[16];
    uint8_t  tail[512 - 8 - 16 * 29 - 16];
} OldDirSector;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
    uint16_t version;             /* 2 = multi-sector layout */
    DirEnt   ents[VFS_MAX_FILES];
    uint8_t  flags[VFS_MAX_FILES];  /* VF_SYSTEM per slot */
    uint8_t  tail[DIR_SECTORS * 512 - 8 - VFS_MAX_FILES * 29 - VFS_MAX_FILES];
} DirSector;                        /* DIR_SECTORS*512 bytes (2 KiB) */

static DirSector s_dir;
static int s_present;

static uint8_t ent_flags(int slot) { return s_dir.flags[slot]; }
static void set_ent_flags(int slot, uint8_t f) { s_dir.flags[slot] = f; }

static uint16_t tail_rd16(int off) {
    uint16_t v; memcpy(&v, s_dir.tail + off, 2); return v;
}
static void tail_wr16(int off, uint16_t v) { memcpy(s_dir.tail + off, &v, 2); }
/* sectors a file occupies (legacy entries report 0 -> 1) */
static int file_secs(int k) {
    if (k < 0 || k >= VFS_MAX_FILES) return 0;
    int n = tail_rd16(TAIL_SECS_OFF + k * 2);
    return n > 0 ? n : 1;
}
static void set_file_size(int k, int secs, int bytes) {
    tail_wr16(TAIL_SECS_OFF + k * 2, (uint16_t)secs);
    tail_wr16(TAIL_BYTES_OFF + k * 2, (uint16_t)bytes);
}

static int find_slot_in(uint8_t parent, const char *name);
static uint16_t alloc_run(int n);   /* fwd: defined near the read/write API */
static void sanitize_legacy_seeds(void);

static int valid_name(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > VFS_NAME_LEN) return 0;
    for (size_t i = 0; i < l; i++)
        if (n[i] < 0x20 || n[i] > 0x7E) return 0;
    return 1;
}

static void flush_dir(void) {
    if (!s_present) return;
    for (int i = 0; i < DIR_SECTORS; i++)
        ata_write_sector(DIR_LBA + i, (const uint8_t *)&s_dir + i * 512);
}

static void read_dir(void) {
    for (int i = 0; i < DIR_SECTORS; i++)
        ata_read_sector(DIR_LBA + i, (uint8_t *)&s_dir + i * 512);
}

/* migrate a legacy 512-byte directory (16 slots, content at LBA 201+)
 * into the multi-sector layout: entries keep their slot indices, file
 * contents are relocated to the new CONTENT_BASE region */
static void migrate_old_dir(const uint8_t *sec0) {
    OldDirSector old;
    memcpy(&old, sec0, sizeof(old));
    memset(&s_dir, 0, sizeof(s_dir));
    s_dir.magic = DIR_MAGIC2;
    s_dir.version = DIR_VER_SIZE;
    uint16_t oldlba[16];
    for (int k = 0; k < 16; k++) {
        s_dir.ents[k] = old.ents[k];
        s_dir.flags[k] = old.flags[k];
        oldlba[k] = old.ents[k].lba;
        if (old.ents[k].name[0] && old.ents[k].type == VFS_FILE)
            s_dir.ents[k].lba = 0;      /* relocated below */
    }
    s_present = 1;
    /* legacy disks predate the system flag (it lived in zero padding):
     * re-protect the seeded system entries so they stay hidden/view-only
     * exactly as originally intended */
    static const struct { const char *n; int t; } seeds[] = {
        { "System", VFS_DIR }, { "Library", VFS_DIR }, { "Voyager", VFS_DIR },
        { "Users", VFS_DIR }, { "readme.txt", VFS_FILE }
    };
    for (int k = 0; k < 16; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (!e->name[0] || e->parent != PARENT_ROOT ||
            (s_dir.flags[k] & VF_SYSTEM)) continue;
        for (int s = 0; s < 5; s++)
            if (e->type == seeds[s].t && !strcmp(e->name, seeds[s].n)) {
                s_dir.flags[k] |= VF_SYSTEM;
                break;
            }
    }
    /* copy each file's content sector to a fresh LBA, then fix the entry */
    for (int k = 0; k < 16; k++) {
        if (!oldlba[k]) continue;
        uint8_t sec[512];
        if (ata_read_sector(oldlba[k], sec) != 0) continue;
        uint16_t nl = alloc_run(1);
        if (!nl) break;
        ata_write_sector(nl, sec);
        s_dir.ents[k].lba = nl;
    }
    flush_dir();
    kprintf("vfs: migrated legacy directory (%d slots)\n", VFS_MAX_FILES);
}

void vfs_init(void) {
    s_present = 0;
    memset(&s_dir, 0, sizeof(s_dir));
    if (!ata_present()) return;
    uint8_t sec0[512];
    if (ata_read_sector(DIR_LBA, sec0) != 0) {
        /* no readable sector: bootstrap an empty directory */
        s_dir.magic = DIR_MAGIC2;
        s_dir.version = DIR_VER_SIZE;
        s_present = 1;
        flush_dir();
        return;
    }
    uint32_t magic;
    uint16_t version;
    memcpy(&magic, sec0, 4);
    memcpy(&version, sec0 + 6, 2);
    if (magic == DIR_MAGIC2 || (magic == DIR_MAGIC && version == 2)) {
        /* DIR_MAGIC+version2 = new layout whose magic was clobbered by an
         * old create path - read it as-is and self-heal the magic */
        read_dir();
        s_dir.magic = DIR_MAGIC2;
        /* keep the on-disk version: the size-metadata upgrade below only
         * zeroes the tail when it is genuinely absent */
        s_present = 1;
    } else if (magic == DIR_MAGIC) {
        migrate_old_dir(sec0);
    } else {
        /* fresh/blank disk: bootstrap an empty directory so creates work */
        s_dir.magic = DIR_MAGIC2;
        s_dir.version = DIR_VER_SIZE;
        s_present = 1;
        flush_dir();
    }
    /* disks written before per-file sizes existed: the tail area is
     * garbage - zero it so every file reads as a single sector */
    if (s_dir.version < DIR_VER_SIZE) {
        memset(s_dir.tail + TAIL_SECS_OFF, 0, 96 + 96);
        s_dir.version = DIR_VER_SIZE;
        flush_dir();
    }
    sanitize_legacy_seeds();
    kprintf("vfs: %s, %d root entries\n", s_present ? "online" : "NO DISK",
            s_present ? vfs_count() : 0);
}

/* one-time heal for disks that migrated before the system flag existed:
 * the boot-time seed entries (System/Library/Voyager/Users/readme.txt)
 * were stored with flags=0 and would show up as normal user files.
 * tail[0] marks that the sweep already ran. */
static void sanitize_legacy_seeds(void) {
    if (!s_present || s_dir.tail[0]) return;
    static const struct { const char *n; int t; } seeds[] = {
        { "System", VFS_DIR }, { "Library", VFS_DIR }, { "Voyager", VFS_DIR },
        { "Users", VFS_DIR }, { "readme.txt", VFS_FILE }
    };
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (!e->name[0] || e->parent != PARENT_ROOT ||
            (s_dir.flags[k] & VF_SYSTEM)) continue;
        for (int s = 0; s < 5; s++)
            if (e->type == seeds[s].t && !strcmp(e->name, seeds[s].n)) {
                s_dir.flags[k] |= VF_SYSTEM;
                break;
            }
    }
    s_dir.tail[0] = 1;
    flush_dir();
}

/* slot index of the i-th USED entry whose parent matches (PARENT_ROOT or
 * a folder slot index); parent_name "" / NULL means desktop root */
static int slot_of_parent(const char *parent_name) {
    if (!parent_name || !parent_name[0]) return PARENT_ROOT;
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (e->name[0] && e->type == VFS_DIR && !strcmp(e->name, parent_name))
            return k;
    }
    return -2;   /* folder not found */
}

static int nth_slot(uint8_t parent, int i) {
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (!e->name[0] || e->parent != parent) continue;
        if (i-- == 0) return k;
    }
    return -1;
}

static int count_slots(uint8_t parent) {
    int n = 0;
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (s_dir.ents[k].name[0] && s_dir.ents[k].parent == parent) n++;
    return n;
}

/* live (display) index of a slot among entries sharing its parent */
static int live_of_slot(int slot) {
    uint8_t p = s_dir.ents[slot].parent;
    int seen = 0;
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        if (!s_dir.ents[k].name[0] || s_dir.ents[k].parent != p) continue;
        if (k == slot) return seen;
        seen++;
    }
    return -1;
}

static void fill_entry(const DirEnt *e, VfsEntry *out) {
    strncpy(out->name, e->name, VFS_NAME_LEN);
    out->name[VFS_NAME_LEN] = 0;
    out->type = e->type;
    out->used = 1;
    out->system = (ent_flags((int)(e - s_dir.ents)) & VF_SYSTEM) ? 1 : 0;
    /* parent live index: -1 for root, else index within ITS parent scope */
    if (e->parent == PARENT_ROOT) {
        out->parent = -1;
    } else {
        /* find the parent slot's live index among root entries */
        out->parent = -1;
        int seen = 0;
        for (int k = 0; k < VFS_MAX_FILES; k++) {
            DirEnt *pe = &s_dir.ents[k];
            if (!pe->name[0] || pe->parent != PARENT_ROOT) continue;
            if (k == e->parent) { out->parent = seen; break; }
            seen++;
        }
    }
}

int vfs_count(void) { return count_slots(PARENT_ROOT); }

int vfs_count_in(const char *folder) {
    int p = slot_of_parent(folder);
    return (p < 0) ? 0 : count_slots((uint8_t)p);
}

const VfsEntry *vfs_entry(int i) {
    int k = nth_slot(PARENT_ROOT, i);
    if (k < 0) return NULL;
    static VfsEntry e;
    fill_entry(&s_dir.ents[k], &e);
    return &e;
}

const VfsEntry *vfs_entry_in(const char *folder, int i) {
    int p = slot_of_parent(folder);
    if (p < 0) return NULL;
    int k = nth_slot((uint8_t)p, i);
    if (k < 0) return NULL;
    static VfsEntry e;
    fill_entry(&s_dir.ents[k], &e);
    return &e;
}

static int find_slot_in(uint8_t parent, const char *name) {
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (e->name[0] && e->parent == parent && !strcmp(e->name, name))
            return k;
    }
    return -1;
}

int vfs_find_in(const char *folder, const char *name) {
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    return (k < 0) ? -1 : live_of_slot(k);
}

int vfs_find(const char *name) { return vfs_find_in(NULL, name); }

/* stable slot index of a ROOT entry (unlike vfs_find's live index, the
 * slot survives create/delete of siblings - used for pinned icon spots) */
int vfs_slot_root(const char *name) {
    if (!s_present || !name || !name[0]) return -1;
    return find_slot_in(PARENT_ROOT, name);
}

int vfs_create_in(const char *name, int type, const char *parent) {
    if (!s_present || !valid_name(name)) return -1;
    int p = slot_of_parent(parent);
    if (p == -2) return -1;                    /* parent folder missing */
    if (p >= 0 && (ent_flags(p) & VF_SYSTEM)) return -1; /* locked */
    uint8_t par = (uint8_t)p;
    if (find_slot_in(par, name) >= 0) return -1;
    int slot = -1;
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (!s_dir.ents[k].name[0]) { slot = k; break; }
    if (slot < 0) return -1;
    DirEnt *e = &s_dir.ents[slot];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, VFS_NAME_LEN);
    e->name[VFS_NAME_LEN] = 0;
    e->type = (uint8_t)type;
    e->parent = par;
    if (type == VFS_FILE) {
        uint16_t lba = alloc_run(1);
        if (!lba) { memset(e, 0, sizeof(*e)); return -1; }
        e->lba = lba;
        uint8_t sec[512];
        memset(sec, 0, sizeof(sec));
        ata_write_sector(lba, sec);
        set_file_size(slot, 1, 0);
    }
    s_dir.magic = DIR_MAGIC2;
    s_dir.version = DIR_VER_SIZE;
    flush_dir();
    return 0;
}

int vfs_create(const char *name, int type) {
    return vfs_create_in(name, type, NULL);
}

int vfs_rename_in(const char *folder, const char *oldname, const char *newname) {
    if (!s_present || !valid_name(newname)) return -1;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, oldname);
    if (k < 0 || (ent_flags(k) & VF_SYSTEM)) return -1;  /* locked */
    if (find_slot_in(s_dir.ents[k].parent, newname) >= 0) return -1;
    DirEnt *e = &s_dir.ents[k];
    memset(e->name, 0, sizeof(e->name));
    strncpy(e->name, newname, VFS_NAME_LEN);
    e->name[VFS_NAME_LEN] = 0;
    flush_dir();
    return 0;
}

int vfs_rename(const char *oldname, const char *newname) {
    return vfs_rename_in(NULL, oldname, newname);
}

/* delete a slot and every descendant (children store parent=slot idx) */
static void delete_tree(int slot) {
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (s_dir.ents[k].name[0] && s_dir.ents[k].parent == (uint8_t)slot)
            delete_tree(k);
    memset(&s_dir.ents[slot], 0, sizeof(DirEnt));
}

int vfs_delete_in(const char *folder, const char *name) {
    if (!s_present) return -1;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    if (k < 0 || (ent_flags(k) & VF_SYSTEM)) return -1;  /* locked */
    delete_tree(k);
    flush_dir();
    return 0;
}

int vfs_delete(const char *name) { return vfs_delete_in(NULL, name); }

/* reparent an entry: drag-into-folder. Rejects moving a folder into
 * itself or one of its own descendants (would create a cycle). */
int vfs_move(const char *name, const char *src_folder, const char *dst_folder) {
    if (!s_present) return -1;
    int sp = slot_of_parent(src_folder), dp = slot_of_parent(dst_folder);
    if (sp < 0 || dp < 0) return -1;
    int k = find_slot_in((uint8_t)sp, name);
    if (k < 0) return -1;
    if (ent_flags(k) & VF_SYSTEM) return -1;          /* locked */
    if (dp >= 0 && (ent_flags(dp) & VF_SYSTEM)) return -1; /* into sys */
    if (s_dir.ents[k].type == VFS_DIR) {
        int cur = dp, guard = 0;
        while (cur != PARENT_ROOT && cur >= 0 && cur < VFS_MAX_FILES &&
               guard++ < VFS_MAX_FILES + 2) {
            if (cur == k) return -1;               /* dst is inside src */
            cur = s_dir.ents[cur].parent;
        }
    }
    if (find_slot_in((uint8_t)dp, name) >= 0) return -1;  /* name clash */
    if ((uint8_t)dp == (uint8_t)sp) return 0;             /* already there */
    s_dir.ents[k].parent = (uint8_t)dp;
    flush_dir();
    return 0;
}

int vfs_parent_name(const char *folder, char *out, int cap) {
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    uint8_t par = s_dir.ents[p].parent;
    if (par == PARENT_ROOT) return 0;
    if (par >= VFS_MAX_FILES) return -1;
    strncpy(out, s_dir.ents[par].name, cap - 1);
    out[cap - 1] = 0;
    return 0;
}

static int lba_of_slot(int k) {
    return (k >= 0 && s_dir.ents[k].type == VFS_FILE) ? s_dir.ents[k].lba : 0;
}

static int read_lba(int lba, int secs, char *buf, int cap) {
    if (!lba || !buf || cap <= 0) return -1;
    if (secs < 1) secs = 1;
    int n = 0;
    for (int s = 0; s < secs && n < cap - 1; s++) {
        uint8_t sec[512];
        if (ata_read_sector(lba + s, sec) != 0) break;
        for (int i = 0; i < 512 && n < cap - 1; i++) {
            if (!sec[i]) { buf[n] = 0; return n; }
            buf[n++] = (char)sec[i];
        }
    }
    buf[n] = 0;
    return n;
}

int vfs_read_in(const char *folder, const char *name, char *buf, int cap) {
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    return read_lba(lba_of_slot(k), file_secs(k), buf, cap);
}

int vfs_read(const char *name, char *buf, int cap) {
    int k = find_slot_in(PARENT_ROOT, name);
    return read_lba(lba_of_slot(k), file_secs(k), buf, cap);
}

/* byte length of a file's content (stored size, else whole sectors) */
int vfs_file_size_in(const char *folder, const char *name) {
    if (!s_present) return -1;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    if (k < 0 || s_dir.ents[k].type != VFS_FILE || !s_dir.ents[k].lba)
        return -1;
    int b = tail_rd16(TAIL_BYTES_OFF + k * 2);
    return b > 0 ? b : file_secs(k) * 512;
}

/* binary-safe read of ALL content sectors (PNGs contain NULs that
 * vfs_read would truncate on). Returns stored byte length. */
int vfs_read_raw_full_in(const char *folder, const char *name,
                         uint8_t *out, int cap) {
    if (!s_present || !out || cap <= 0) return -1;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    if (k < 0 || s_dir.ents[k].type != VFS_FILE || !s_dir.ents[k].lba)
        return -1;
    if (ent_flags(k) & VF_SYSTEM) return -1;   /* system volume is read-only
                                                  via sysvol, not raw sectors */
    int secs = file_secs(k), lba = s_dir.ents[k].lba, got = 0;
    for (int s = 0; s < secs && got + 512 <= cap; s++) {
        if (ata_read_sector(lba + s, out + got) != 0) break;
        got += 512;
    }
    int bytes = tail_rd16(TAIL_BYTES_OFF + k * 2);
    return bytes > 0 ? bytes : got;
}

/* binary-safe read: returns the raw 512-byte content sector (PNGs and
 * other binary files contain NULs that vfs_read would truncate on) */
int vfs_read_raw_in(const char *folder, const char *name, uint8_t *out) {
    if (!s_present || !out) return -1;
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    int k = find_slot_in((uint8_t)p, name);
    if (k < 0 || s_dir.ents[k].type != VFS_FILE || !s_dir.ents[k].lba)
        return -1;
    if (ent_flags(k) & VF_SYSTEM) return -1;   /* system volume is read-only
                                                  via sysvol, not raw sectors */
    return ata_read_sector(s_dir.ents[k].lba, out);
}

/* is LBA `cand` covered by any file's sector run? */
static int lba_covered(uint16_t cand) {
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        DirEnt *e = &s_dir.ents[k];
        if (e->type != VFS_FILE || !e->lba) continue;
        int n = file_secs(k);
        for (int s = 0; s < n; s++)
            if (e->lba + s == cand) return 1;
    }
    return 0;
}

/* largest run of consecutive free content sectors - a file needs one
 * contiguous run, so this is the real per-file ceiling. Also usable as
 * a disk-space check before saving/downloading. */
int vfs_free_run_sectors(void) {
    int best = 0, cur = 0;
    for (int l = 0; l < MAX_SECTORS; l++) {
        if (lba_covered(CONTENT_BASE + l)) {
            if (cur > best) best = cur;
            cur = 0;
        } else cur++;
    }
    if (cur > best) best = cur;
    return best;
}

/* first run of `n` consecutive free content LBAs (0 = none) */
static uint16_t alloc_run(int n) {
    if (n < 1) n = 1;
    for (int l = 0; l + n <= MAX_SECTORS; l++) {
        uint16_t cand = CONTENT_BASE + l;
        int ok = 1;
        for (int s = 0; s < n && ok; s++)
            if (lba_covered(cand + s)) ok = 0;
        if (ok) return cand;
    }
    return 0;
}

static int write_file_slot(int k, const char *buf, int len) {
    if (k < 0 || k >= VFS_MAX_FILES) return -1;
    if (s_dir.ents[k].type != VFS_FILE) return -1;
    /* protected system entries are view-only: writes are refused */
    if (ent_flags(k) & VF_SYSTEM) return -1;
    if (len < 0) return -1;
    int maxb = MAX_SECTORS * 512;
    if (len > maxb) len = maxb;
    int n = (len + 511) / 512;
    if (n < 1) n = 1;
    uint16_t lba = s_dir.ents[k].lba;
    /* reuse the existing run when the sector count matches */
    if (!lba || file_secs(k) != n) {
        uint16_t nl = alloc_run(n);
        if (!nl) return -1;
        lba = nl;
    }
    const uint8_t *p = (const uint8_t *)buf;
    for (int s = 0; s < n; s++) {
        uint8_t sec[512];
        memset(sec, 0, sizeof(sec));
        int chunk = len - s * 512;
        if (chunk > 512) chunk = 512;
        if (chunk > 0) memcpy(sec, p + s * 512, (size_t)chunk);
        if (ata_write_sector(lba + s, sec) != 0) return -1;
    }
    s_dir.ents[k].lba = lba;
    set_file_size(k, n, len);
    flush_dir();
    return 0;
}

static int write_lba_slot(int k, const char *buf, int len) {
    return write_file_slot(k, buf, len);
}

int vfs_write_in(const char *folder, const char *name, const char *buf, int len) {
    int p = slot_of_parent(folder);
    if (p < 0) return -1;
    return write_lba_slot(find_slot_in((uint8_t)p, name), buf, len);
}

int vfs_write(const char *name, const char *buf, int len) {
    int k = find_slot_in(PARENT_ROOT, name);
    return write_lba_slot(k, buf, len);
}

/* ================= foreign Voyager volumes =================
 * Same on-disk format as the boot volume, living on another attached
 * disk (USB, second VDI, VM-added drive). media index matches
 * ata_media_info ordering. Supports browse + file copy both ways so
 * users can drag files on and off the disk. */

static DirSector s_fdir;
static int       s_fpresent;
static int       s_fmidx = -1;

static uint16_t f_tail_rd16(int off) {
    uint16_t v; memcpy(&v, s_fdir.tail + off, 2); return v;
}
static void f_tail_wr16(int off, uint16_t v) { memcpy(s_fdir.tail + off, &v, 2); }
static int f_file_secs(int k) {
    int n = f_tail_rd16(TAIL_SECS_OFF + k * 2);
    return n > 0 ? n : 1;
}

/* is media  a Voyager volume? caches the directory on success */
int vfsx_probe(int midx) {
    s_fpresent = 0; s_fmidx = -1;
    uint8_t sec[512];
    if (ata_read_sector_dev(midx, DIR_LBA, sec) != 0) return 0;
    uint32_t magic; memcpy(&magic, sec, 4);
    if (magic != DIR_MAGIC2 && magic != DIR_MAGIC) return 0;
    memcpy(&s_fdir, sec, 512);
    for (int i = 1; i < DIR_SECTORS; i++)
        ata_read_sector_dev(midx, DIR_LBA + i,
                            (uint8_t *)&s_fdir + i * 512);
    s_fpresent = 1; s_fmidx = midx;
    return 1;
}

static int fx_live(void) { return s_fpresent && s_fmidx >= 0; }

/* children of dirslot (-1 = root) - returns live entry count */
int vfsx_count(int midx, int dirslot) {
    if (!fx_live() || midx != s_fmidx) return 0;
    uint8_t par = (dirslot < 0) ? PARENT_ROOT : (uint8_t)dirslot;
    int n = 0;
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (s_fdir.ents[k].name[0] && s_fdir.ents[k].parent == par) n++;
    return n;
}

/* i-th child of dirslot -> out (name/type/parent/used); also returns
 * the stable slot index via *slot (needed for read/write/drill) */
int vfsx_get(int midx, int dirslot, int i, VfsEntry *out, int *slot) {
    if (!fx_live() || midx != s_fmidx || !out) return 0;
    uint8_t par = (dirslot < 0) ? PARENT_ROOT : (uint8_t)dirslot;
    for (int k = 0; k < VFS_MAX_FILES; k++) {
        if (!s_fdir.ents[k].name[0] || s_fdir.ents[k].parent != par) continue;
        if (i-- > 0) continue;
        memset(out, 0, sizeof(*out));
        strncpy(out->name, s_fdir.ents[k].name, VFS_NAME_LEN);
        out->name[VFS_NAME_LEN] = 0;
        out->type = s_fdir.ents[k].type;
        out->parent = s_fdir.ents[k].parent == PARENT_ROOT ? -1
                                                          : s_fdir.ents[k].parent;
        out->used = 1;
        if (slot) *slot = k;
        return 1;
    }
    return 0;
}

/* parent slot of a directory slot (-1 = root's parent) */
int vfsx_parent(int midx, int dirslot) {
    if (!fx_live() || midx != s_fmidx || dirslot < 0 ||
        dirslot >= VFS_MAX_FILES) return -2;
    uint8_t p = s_fdir.ents[dirslot].parent;
    return p == PARENT_ROOT ? -1 : (int)p;
}

/* read a file's content bytes (cap = buffer size) */
int vfsx_read(int midx, int slot, uint8_t *out, int cap) {
    if (!fx_live() || midx != s_fmidx || slot < 0 || slot >= VFS_MAX_FILES ||
        !out || cap <= 0) return -1;
    if (s_fdir.ents[slot].type != VFS_FILE || !s_fdir.ents[slot].lba) return -1;
    int want = f_tail_rd16(TAIL_BYTES_OFF + slot * 2);
    int secs = f_file_secs(slot);
    if (want <= 0) want = secs * 512;
    if (want > cap) want = cap;
    uint16_t lba = s_fdir.ents[slot].lba;
    int got = 0;
    for (int s = 0; s < secs && got < want; s++) {
        uint8_t sec[512];
        if (ata_read_sector_dev(midx, lba + s, sec) != 0) break;
        int chunk = want - got; if (chunk > 512) chunk = 512;
        memcpy(out + got, sec, (size_t)chunk);
        got += chunk;
    }
    return got;
}

/* write a new file into dirslot (-1 = root). Returns 0 on success. */
int vfsx_write(int midx, int dirslot, const char *name,
               const char *buf, int len) {
    if (!fx_live() || midx != s_fmidx || !valid_name(name) || !buf ||
        len < 0) return -1;
    if (dirslot >= 0 && (dirslot >= VFS_MAX_FILES ||
        s_fdir.ents[dirslot].type != VFS_DIR)) return -1;
    /* name must be free in this dir */
    uint8_t par = (dirslot < 0) ? PARENT_ROOT : (uint8_t)dirslot;
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (s_fdir.ents[k].name[0] && s_fdir.ents[k].parent == par &&
            !strcmp(s_fdir.ents[k].name, name)) return -1;
    /* free slot */
    int slot = -1;
    for (int k = 0; k < VFS_MAX_FILES; k++)
        if (!s_fdir.ents[k].name[0]) { slot = k; break; }
    if (slot < 0) return -1;
    /* contiguous free content run (same pool bounds as boot volume) */
    int n = (len + 511) / 512; if (n < 1) n = 1;
    uint16_t lba = 0;
    for (int l = 0; l + n <= MAX_SECTORS && !lba; l++) {
        uint16_t cand = CONTENT_BASE + l;
        int ok = 1;
        for (int s = 0; s < n && ok; s++) {
            uint16_t c = cand + s;
            for (int k = 0; k < VFS_MAX_FILES && ok; k++) {
                if (!s_fdir.ents[k].lba ||
                    s_fdir.ents[k].type != VFS_FILE) continue;
                int fs = f_file_secs(k);
                if (c >= s_fdir.ents[k].lba && c < s_fdir.ents[k].lba + fs)
                    ok = 0;
            }
        }
        if (ok) lba = cand;
    }
    if (!lba) return -1;
    /* write content sectors */
    const uint8_t *p = (const uint8_t *)buf;
    for (int s = 0; s < n; s++) {
        uint8_t sec[512]; memset(sec, 0, sizeof(sec));
        int chunk = len - s * 512; if (chunk > 512) chunk = 512;
        if (chunk > 0) memcpy(sec, p + s * 512, (size_t)chunk);
        if (ata_write_sector_dev(midx, lba + s, sec) != 0) return -1;
    }
    /* fill the directory entry + size metadata, flush all 4 sectors */
    DirEnt *e = &s_fdir.ents[slot];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, VFS_NAME_LEN); e->name[VFS_NAME_LEN] = 0;
    e->type = VFS_FILE; e->parent = par; e->lba = lba;
    f_tail_wr16(TAIL_SECS_OFF + slot * 2, (uint16_t)n);
    f_tail_wr16(TAIL_BYTES_OFF + slot * 2, (uint16_t)len);
    for (int i = 0; i < DIR_SECTORS; i++)
        ata_write_sector_dev(midx, DIR_LBA + i,
                             (const uint8_t *)&s_fdir + i * 512);
    return 0;
}

/* byte length of a foreign file (0 = not a file / unknown) */
int vfsx_size(int midx, int slot) {
    if (!fx_live() || midx != s_fmidx || slot < 0 ||
        slot >= VFS_MAX_FILES) return 0;
    if (s_fdir.ents[slot].type != VFS_FILE || !s_fdir.ents[slot].lba)
        return 0;
    int b = f_tail_rd16(TAIL_BYTES_OFF + slot * 2);
    if (b <= 0) b = f_file_secs(slot) * 512;
    return b;
}
