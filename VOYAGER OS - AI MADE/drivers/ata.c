/* ATA PIO (Programmed I/O) driver - both IDE channels, master+slave.
 * Primary: 0x1F0-0x1F7 / alt 0x3F6. Secondary: 0x170-0x177 / alt 0x376.
 * LBA28 addressing, one sector at a time. Pure polling.
 *
 * The boot/preferred disk (first present in probe order) backs the VFS
 * and settings store via ata_read_sector/ata_write_sector. A separate
 * device table (ata_dev_*) exposes every attached drive - internal,
 * USB-storage, optical (ATAPI), or VM-added - so the GUI can show
 * removable media when it appears or disappears. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ata.h"
#include "../kernel/k.h"

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_PACKET   0xA1
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30

#define ATA_ST_BUSY  0x80
#define ATA_ST_DRQ   0x08
#define ATA_ST_ERR   0x01

/* one slot = one (channel, master/slave) pair */
typedef struct {
    uint16_t base;      /* command block base (0x1F0 / 0x170) */
    uint16_t alt;       /* alternate status (0x3F6 / 0x376) */
    uint8_t  devsel;    /* 0xA0 master / 0xB0 slave */
    int      present;
    int      removable;
    int      atapi;
    uint32_t sectors;   /* capacity in 512-byte sectors (0 = unknown) */
    char     model[42];
} AtaSlot;

static AtaSlot g_slots[4];
static int     g_boot = -1;    /* slot index backing VFS/settings */

/* probe order keeps the historical preference: primary slave first
 * (the settings VDI), then primary master, then secondary pair */
static const AtaSlot k_probe[4] = {
    { 0x1F0, 0x3F6, 0xB0, 0, 0, 0, 0, "" },   /* primary slave */
    { 0x1F0, 0x3F6, 0xA0, 0, 0, 0, 0, "" },   /* primary master */
    { 0x170, 0x376, 0xB0, 0, 0, 0, 0, "" },   /* secondary slave */
    { 0x170, 0x376, 0xA0, 0, 0, 0, 0, "" },   /* secondary master */
};

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outsw(uint16_t port, const uint16_t *buf, int n) {
    __asm__ volatile("rep outsw" :: "d"(port), "S"(buf), "c"(n) : "memory");
}
static inline void insw(uint16_t port, uint16_t *buf, int n) {
    __asm__ volatile("rep insw" : "=D"(buf) : "d"(port), "D"(buf), "c"(n) : "memory");
}

#define ATA_DATA(s)   ((s)->base + 0)
#define ATA_ERROR(s)  ((s)->base + 1)
#define ATA_SECCNT(s) ((s)->base + 2)
#define ATA_LBA0(s)   ((s)->base + 3)
#define ATA_LBA1(s)   ((s)->base + 4)
#define ATA_LBA2(s)   ((s)->base + 5)
#define ATA_DRIVE(s)  ((s)->base + 6)
#define ATA_STATUS(s) ((s)->base + 7)
#define ATA_CMD(s)    ((s)->base + 7)

static int ata_wait_not_busy(AtaSlot *s) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS(s));
        if (!(st & ATA_ST_BUSY)) return (st & ATA_ST_ERR) ? -1 : 0;
    }
    return -1;
}

static int ata_wait_drq(AtaSlot *s) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS(s));
        if (st & ATA_ST_ERR) return -1;
        if (st & ATA_ST_DRQ) return 0;
        if (!(st & ATA_ST_BUSY) && !(st & ATA_ST_DRQ)) return -2;
    }
    return -1;
}

static void ata_select(AtaSlot *s, uint32_t lba) {
    outb(ATA_SECCNT(s), 1);
    outb(ATA_LBA0(s), lba & 0xFF);
    outb(ATA_LBA1(s), (lba >> 8) & 0xFF);
    outb(ATA_LBA2(s), (lba >> 16) & 0xFF);
    outb(ATA_DRIVE(s), s->devsel | (((lba) >> 24) & 0x0F));
}

/* IDENTIFY DEVICE (0xEC); falls back to PACKET IDENTIFY (0xA1) for
 * ATAPI optical drives. Returns 1 and fills buf on success. */
static int ata_identify(AtaSlot *s, uint16_t *buf) {
    memset(buf, 0, 512);
    if (ata_wait_not_busy(s) != 0) return 0;
    outb(ATA_DRIVE(s), s->devsel);
    outb(ATA_CMD(s), ATA_CMD_IDENTIFY);
    /* let the device actually process the command before sampling
     * status - reading it back immediately can return stale DRDY */
    ata_wait_not_busy(s);
    uint8_t st = inb(ATA_STATUS(s));
    if (st == 0) return 0;                       /* no device on this sel */
    if (ata_wait_drq(s) == 0) {
        insw(ATA_DATA(s), buf, 256);
        ata_wait_not_busy(s);
        if (buf[0] || buf[1] || buf[27]) return 1;   /* ATA device */
    }
    /* ATA IDENTIFY failed or returned nothing - could be ATAPI optical:
     * clear the error status (a failed command latches ERR until the
     * ERROR register is read), then retry with PACKET IDENTIFY */
    memset(buf, 0, 512);
    s->atapi = 0;
    (void)inb(ATA_ERROR(s));
    /* no wait_not_busy gate here: a latched ERR bit from the failed
     * 0xEC would abort the retry even though PACKET is a fresh command */
    outb(ATA_DRIVE(s), s->devsel);
    /* 12-byte IDENTIFY PACKET DEVICE CDB, sent as 6 little-endian words:
     * A1 00 00 00 00 40 00 00 00 00 00 00 */
    static const uint16_t pkt[6] = {
        0x00A1, 0x0000, 0x4000, 0x0000, 0x0000, 0x0000
    };
    outsw(ATA_DATA(s), pkt, 6);
    outb(ATA_CMD(s), ATA_CMD_PACKET);
    /* error-tolerant DRQ wait: the CD may still show a stale ERR bit
     * from the failed 0xEC - keep polling, clearing ERROR, until the
     * packet data (or a real abort) arrives */
    int got = 0;
    for (int i = 0; i < 1000000 && !got; i++) {
        uint8_t st2 = inb(ATA_STATUS(s));
        if (st2 & ATA_ST_DRQ) got = 1;
        else if (st2 & ATA_ST_ERR) (void)inb(ATA_ERROR(s));
        else if (!(st2 & ATA_ST_BUSY)) break;
    }
    if (!got) return 0;
    insw(ATA_DATA(s), buf, 256);
    ata_wait_not_busy(s);
    s->atapi = 1;
    return (buf[0] || buf[1] || buf[27]) ? 1 : 0;
}

static void ata_decode_model(AtaSlot *s, const uint16_t *buf) {
    /* words 27-46: 20 words = 40 chars, each word byte-swapped */
    char m[41];
    for (int i = 0; i < 20; i++) {
        uint16_t w = buf[27 + i];
        m[i * 2]     = (char)(w >> 8);
        m[i * 2 + 1] = (char)(w & 0xFF);
    }
    m[40] = 0;
    /* trim trailing spaces */
    int l = 40;
    while (l > 0 && m[l - 1] == ' ') m[--l] = 0;
    strncpy(s->model, m, sizeof(s->model) - 1);
    s->model[sizeof(s->model) - 1] = 0;
    /* capacity: words 60-61 (LBA28); ATAPI may use words 80-81 */
    uint32_t cap = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
    if (!cap) cap = (uint32_t)buf[80] | ((uint32_t)buf[81] << 16);
    s->sectors = cap;
    /* word 0 bit 7 = removable media; word 2 bits 13:12 = device type
     * (0 = direct access, 1 = CD/DVD optical, ...) */
    s->removable = (buf[0] & 0x0080) ? 1 : 0;
}

/* probe every slot and refresh the device table. Called at boot and
 * periodically so hot-added/removed media (USB, VM disks, optical)
 * shows up without a reboot. */
void ata_poll(void) {
    uint16_t buf[256];
    /* signature of the whole table - log only when the attached set or
     * any capacity/type actually changes (hotplug), not every poll */
    uint32_t sig = 0;
    for (int i = 0; i < 4; i++) {
        AtaSlot *s = &g_slots[i];
        *s = k_probe[i];
        if (ata_identify(s, buf)) {
            s->present = 1;
            ata_decode_model(s, buf);
            sig = sig * 33 + (uint32_t)(i + 1) * 0x85EBCA6Bu +
                  (s->sectors ^ ((uint32_t)s->atapi << 28) ^
                   ((uint32_t)s->removable << 29));
        }
    }
    static uint32_t last_sig;
    static int logged_once;
    if (!logged_once || sig != last_sig) {
        logged_once = 1;
        last_sig = sig;
        for (int i = 0; i < 4; i++) {
            AtaSlot *s = &g_slots[i];
            if (!s->present) continue;
            kprintf("ata: slot%d %s %s%s cap=%u sectors model='%s'\n", i,
                    s->devsel == 0xB0 ? "slave" : "master",
                    s->base == 0x1F0 ? "prim" : "sec",
                    s->atapi ? " atapi" : "",
                    (unsigned)s->sectors, s->model);
        }
        kprintf("ata: media poll - %d device(s)\n", ata_media_count());
    }
    /* boot disk = first present slot in probe order */
    g_boot = -1;
    for (int i = 0; i < 4; i++)
        if (g_slots[i].present) { g_boot = i; break; }
}

int ata_present(void) {
    if (g_boot < 0 &&
        !g_slots[0].present && !g_slots[1].present &&
        !g_slots[2].present && !g_slots[3].present)
        ata_poll();
    return g_boot >= 0;
}

static uint8_t g_eject[4];   /* user-ejected slots: hidden until refresh */

int ata_media_count(void) {
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (g_slots[i].present && !g_eject[i]) n++;
    return n;
}

int ata_media_info(int idx, AtaMediaInfo *out) {
    int seen = -1;
    for (int i = 0; i < 4; i++) {
        if (!g_slots[i].present || g_eject[i]) continue;
        seen++;
        if (seen == idx) {
            if (!out) return 1;
            out->removable = g_slots[i].removable;
            out->atapi     = g_slots[i].atapi;
            out->sectors   = g_slots[i].sectors;
            strncpy(out->model, g_slots[i].model, sizeof(out->model) - 1);
            out->model[sizeof(out->model) - 1] = 0;
            return 1;
        }
    }
    return 0;
}

/* sector I/O on an arbitrary attached device (media index, same order
 * as ata_media_info) - used to browse Voyager volumes on USB/second
 * disks, not just the boot volume */
static AtaSlot *ata_slot_for_media(int midx) {
    int seen = -1;
    for (int i = 0; i < 4; i++) {
        if (!g_slots[i].present || g_eject[i]) continue;
        seen++;
        if (seen == midx) return &g_slots[i];
    }
    return NULL;
}

/* eject a detected disk: optical drives get a real START STOP UNIT
 * eject packet (best effort - QEMU may ignore it), every device is
 * then hidden from the media list until ata_media_refresh() */
int ata_eject_media(int midx) {
    AtaSlot *s = ata_slot_for_media(midx);
    if (!s) return -1;
    if (s->atapi && ata_wait_not_busy(s) == 0) {
        outb(ATA_DRIVE(s), s->devsel);
        /* START STOP UNIT CDB: opcode 0x1B, byte5 = 0x02 (eject) */
        static const uint16_t pkt[6] = {
            0x001B, 0x0000, 0x0000, 0x0002, 0x0000, 0x0000
        };
        outsw(ATA_DATA(s), pkt, 6);
        outb(ATA_CMD(s), ATA_CMD_PACKET);
        ata_wait_not_busy(s);
        (void)inb(ATA_ERROR(s));
    }
    for (int i = 0; i < 4; i++)
        if (&g_slots[i] == s) { g_eject[i] = 1; break; }
    return 0;
}

/* rescan: clear user ejections and re-poll the buses */
void ata_media_refresh(void) {
    memset(g_eject, 0, sizeof(g_eject));
    ata_poll();
}

int ata_read_sector_dev(int midx, uint32_t lba, void *buf) {
    AtaSlot *s = ata_slot_for_media(midx);
    if (!s) return -1;
    if (ata_wait_not_busy(s) != 0) return -1;
    ata_select(s, lba);
    outb(ATA_CMD(s), ATA_CMD_READ);
    if (ata_wait_drq(s) != 0) return -1;
    insw(ATA_DATA(s), (uint16_t *)buf, 256);
    return ata_wait_not_busy(s);
}

int ata_write_sector_dev(int midx, uint32_t lba, const void *buf) {
    AtaSlot *s = ata_slot_for_media(midx);
    if (!s) return -1;
    if (ata_wait_not_busy(s) != 0) return -1;
    ata_select(s, lba);
    outb(ATA_CMD(s), ATA_CMD_WRITE);
    if (ata_wait_drq(s) != 0) return -1;
    outsw(ATA_DATA(s), (const uint16_t *)buf, 256);
    return ata_wait_not_busy(s);
}

int ata_read_sector(uint32_t lba, void *buf) {
    if (g_boot < 0) return -1;
    AtaSlot *s = &g_slots[g_boot];
    if (ata_wait_not_busy(s) != 0) return -1;
    ata_select(s, lba);
    outb(ATA_CMD(s), ATA_CMD_READ);
    if (ata_wait_drq(s) != 0) return -1;
    insw(ATA_DATA(s), (uint16_t *)buf, 256);
    return ata_wait_not_busy(s);
}

int ata_write_sector(uint32_t lba, const void *buf) {
    if (g_boot < 0) return -1;
    AtaSlot *s = &g_slots[g_boot];
    if (ata_wait_not_busy(s) != 0) return -1;
    ata_select(s, lba);
    outb(ATA_CMD(s), ATA_CMD_WRITE);
    if (ata_wait_drq(s) != 0) return -1;
    outsw(ATA_DATA(s), (const uint16_t *)buf, 256);
    return ata_wait_not_busy(s);
}
