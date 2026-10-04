/* diskio.c — FatFs's disk layer, over sd.c (design.md §10.1).
 *
 * One drive, the SD card, used from core 1 only.
 */

#include <stddef.h>

#include "ff.h"
#include "diskio.h"

#include "sd.h"

static DSTATUS s_status = STA_NOINIT;

/* A transfer that failed may be a card pulled mid-job: the drive is
 * uninitialised until disk_initialize runs sd_init again, so FatFs never
 * reuses a mount across it. Each job mounts afresh anyway (storage.h). */
static DRESULT failed(void) {
    s_status |= STA_NOINIT;
    return RES_ERROR;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!sd_present()) s_status |= STA_NODISK | STA_NOINIT;
    return s_status;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    s_status = (sd_init(NULL, NULL) == SD_OK) ? 0 : STA_NOINIT;
    if (!sd_present()) s_status |= STA_NODISK;
    return s_status;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (s_status & STA_NOINIT) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (sd_read((uint32_t)sector + i, buff + i * SD_BLOCK) != SD_OK) return failed();
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (s_status & STA_NOINIT) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (sd_write((uint32_t)sector + i, buff + i * SD_BLOCK) != SD_OK) return failed();
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)buff;
    if (pdrv != 0) return RES_PARERR;
    if (s_status & STA_NOINIT) return RES_NOTRDY;
    /* Single-block writes complete before sd_write returns, so there is
     * nothing to flush; mkfs and trim, which want the rest, are off. */
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
