/* System volume - see sysvol.h. Parses the /boot/sysvol.txt catalog that
 * tools/sysvol.sh bakes into the ISO, and matches module-backed files to
 * their Limine module contents. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sysvol.h"
#include "k.h"
#include "limine.h"

static SysVolEnt s_vol[SYSVOL_MAX];
static int       s_n;
static int       s_ready;

/* Limine module request (filled by the bootloader) */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL,
};

static void sysvol_parse(void) {
    s_ready = 1;
    struct limine_module_response *r =
        (struct limine_module_response *)module_request.response;
    if (!r) return;

    /* find the catalog module: path ending in "/boot/sysvol.txt" */
    const char *cat = NULL;
    uint64_t catlen = 0;
    for (uint64_t i = 0; i < r->module_count; i++) {
        struct limine_file *f = r->modules[i];
        const char *p = f->path ? (const char *)f->path : "";
        size_t pl = strlen(p), cl = strlen("/boot/sysvol.txt");
        if (pl >= cl && !memcmp(p + pl - cl, "/boot/sysvol.txt", cl)) {
            cat = (const char *)f->address;
            catlen = f->size;
            break;
        }
    }
    if (!cat) { kprintf("sysvol: catalog module not found\n"); return; }

    /* parse "D|path" / "F|path|size[|M]" lines */
    uint64_t pos = 0;
    while (pos < catlen && s_n < SYSVOL_MAX) {
        uint64_t eol = pos;
        while (eol < catlen && cat[eol] != '\n') eol++;
        uint64_t len = eol - pos;
        if (len > 2 && cat[pos] != '#') {
            char line[160];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, cat + pos, len);
            line[len] = 0;
            char *p1 = strchr(line, '|');
            if (p1) {
                *p1 = 0;
                char *p2 = strchr(p1 + 1, '|');
                char *p3 = p2 ? strchr(p2 + 1, '|') : NULL;
                /* terminate the path field: without this "full" keeps the
                 * "|size|B" tail and every file name/path is corrupted */
                if (p2) *p2 = 0;
                int is_dir = (line[0] == 'D');
                const char *full = p1 + 1;      /* "boot/assets/x.png" */
                SysVolEnt *e = &s_vol[s_n];
                memset(e, 0, sizeof(*e));
                /* name = basename, dir = parent path, path = full.
                 * Names are capped at VFS_NAME_LEN (the on-disk VFS slot
                 * size can't grow), so when a long name is cut we keep
                 * its extension - "x.mp3" must stay ".mp3" or the file
                 * type checks (audio/png/text) all fail */
                const char *base = strrchr(full, '/');
                base = base ? base + 1 : full;
                size_t blen = strlen(base);
                if (blen > VFS_NAME_LEN) {
                    const char *dot = strrchr(base, '.');
                    size_t el = (dot && dot > base) ? strlen(dot) : 0;
                    if (el > 1 && el <= 9) {
                        size_t head = VFS_NAME_LEN - el;
                        memcpy(e->v.name, base, head);
                        memcpy(e->v.name + head, dot, el);
                        e->v.name[VFS_NAME_LEN] = 0;
                    } else {
                        strncpy(e->v.name, base, VFS_NAME_LEN);
                        e->v.name[VFS_NAME_LEN] = 0;
                    }
                } else {
                    strncpy(e->v.name, base, VFS_NAME_LEN);
                    e->v.name[VFS_NAME_LEN] = 0;
                }
                size_t fulllen = strlen(full), baselen = strlen(base);
                if (fulllen > baselen) {
                    size_t dl = fulllen - baselen - 1;
                    if (dl >= sizeof(e->dir)) dl = sizeof(e->dir) - 1;
                    memcpy(e->dir, full, dl);
                    e->dir[dl] = 0;
                }
                strncpy(e->path, full, sizeof(e->path) - 1);
                e->v.type = is_dir ? VFS_DIR : VFS_FILE;
                e->v.system = 1;
                e->v.used = 1;
                e->v.parent = -1;
                e->size = (is_dir || !p2) ? 0 : (uint32_t)strtoull(p2 + 1, 10);
                e->has_text = p3 && p3[1] == 'M';
                e->has_blob = p3 && p3[1] == 'B';
                s_n++;
            }
        }
        pos = eol + 1;
    }
    kprintf("sysvol: %d entries from ISO catalog\n", s_n);
}

void sysvol_init(void) { if (!s_ready) sysvol_parse(); }
int  sysvol_ready(void) { sysvol_init(); return s_n > 0; }

int sysvol_count(const char *dir) {
    sysvol_init();
    int n = 0;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_vol[i].dir, dir ? dir : "")) n++;
    return n;
}

const VfsEntry *sysvol_entry(const char *dir, int i) {
    sysvol_init();
    for (int k = 0; k < s_n; k++) {
        if (!strcmp(s_vol[k].dir, dir ? dir : "") && i-- == 0)
            return &s_vol[k].v;
    }
    return NULL;
}

const SysVolEnt *sysvol_ent(const char *dir, int i) {
    sysvol_init();
    for (int k = 0; k < s_n; k++) {
        if (!strcmp(s_vol[k].dir, dir ? dir : "") && i-- == 0)
            return &s_vol[k];
    }
    return NULL;
}

const SysVolEnt *sysvol_lookup(const char *path) {
    sysvol_init();
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_vol[i].path, path)) return &s_vol[i];
    return NULL;
}

/* shared Limine-module lookup: module paths end with "/" + ISO path */
static const void *sysvol_module(const char *path, uint32_t *len) {
    struct limine_module_response *r =
        (struct limine_module_response *)module_request.response;
    if (!r) return NULL;
    char suffix[SYSVOL_PATH + 2];
    snprintf(suffix, sizeof(suffix), "/%s", path);
    size_t sl = strlen(suffix);
    for (uint64_t i = 0; i < r->module_count; i++) {
        struct limine_file *f = r->modules[i];
        const char *p = f->path ? (const char *)f->path : "";
        size_t pl = strlen(p);
        if (pl >= sl && !memcmp(p + pl - sl, suffix, sl)) {
            if (len) *len = (uint32_t)f->size;
            return f->address;
        }
    }
    return NULL;
}

int sysvol_text(const SysVolEnt *e, const char **ptr, uint32_t *len) {
    if (!e || !e->has_text) return -1;
    const void *m = sysvol_module(e->path, len);
    if (!m) return -1;
    *ptr = (const char *)m;
    return 0;
}

int sysvol_blob(const SysVolEnt *e, const uint8_t **ptr, uint32_t *len) {
    if (!e || !e->has_blob) return -1;
    const void *m = sysvol_module(e->path, len);
    if (!m) return -1;
    *ptr = (const uint8_t *)m;
    return 0;
}
