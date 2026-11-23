/* ATA PIO (Programmed I/O) driver - both IDE channels, master+slave.
 * Primary: 0x1F0-0x1F7 / alt 0x3F6. Secondary: 0x170-0x177 / alt 0x376.
 * Polling only, no interrupts, 28-bit LBA, 512-byte sectors.
 * Used for persistent OS settings + the VFS disk, and for detecting
 * attached/removable media (USB, optical, VM-added drives). */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  ata_present(void);                          /* 1 if drive responds */
int  ata_read_sector(uint32_t lba, void *buf);   /* returns 0 on success */
int  ata_write_sector(uint32_t lba, const void *buf);

/* removable/attached-media table: every IDE device on both channels
 * (internal disks, USB storage, optical, VM-added drives). ata_poll()
 * refreshes it - call periodically to catch hotplug. */
typedef struct {
    int      removable;    /* device reports removable media */
    int      atapi;        /* optical/CD/DVD-class device */
    uint32_t sectors;      /* capacity in 512-byte sectors (0 = unknown) */
    char     model[42];    /* IDENTIFY model string, may be empty */
} AtaMediaInfo;

void ata_poll(void);
int  ata_media_count(void);                     /* attached devices */
int  ata_media_info(int idx, AtaMediaInfo *out);/* idx in [0,count) */
/* raw sector I/O on any attached device (media index) - for browsing
 * Voyager volumes that live on non-boot disks */
int  ata_read_sector_dev(int midx, uint32_t lba, void *buf);
int  ata_write_sector_dev(int midx, uint32_t lba, const void *buf);
int  ata_eject_media(int midx);      /* hide disk until refresh */
void ata_media_refresh(void);       /* clear ejections + repoll */

#ifdef __cplusplus
}
#endif

#endif
