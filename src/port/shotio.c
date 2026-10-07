/* shotio.c — screenshots to the card (shotio.h, design.md §12). */

#include "shotio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "config.h"
#include "display.h"
#include "handoff.h"
#include "kbd.h"
#include "log.h"
#include "sd.h"
#include "shot.h"
#include "storage.h"

/* Rows a write: 15 sectors of the panel's width, so most of the file
 * goes to the card a run of whole sectors at a time. */
#define SHOT_ROWS 8u
_Static_assert(ACE_PANEL_H % SHOT_ROWS == 0, "the rows must divide the panel");

/* The keyboard at the live loop's rate between writes, which also feeds
 * the MCU's 2.5 s bus watchdog through a slow card (hardware-notes.md
 * §6.1, §7.1). */
#define POLL_US 33333u

static FIL      s_f;
static uint8_t  s_buf[SHOT_ROWS * SHOT_BMP_ROW(ACE_PANEL_W)];
static uint16_t s_px[ACE_PANEL_W];
static char     s_said[ACE_TEXT_COLS + 1];

static FRESULT write_all(const void *p, UINT n) {
    UINT put = 0;
    FRESULT fr = f_write(&s_f, p, n, &put);
    return fr == FR_OK && put != n ? FR_DENIED : fr;
}

/* The highest SHOTnnnn.bmp in the directory, 0 if none. A .new left by
 * a shot cut short is not counted, and is overwritten. */
static unsigned highest(void) {
    DIR d;
    FILINFO fi;
    unsigned top = 0;
    if (f_opendir(&d, SHOTIO_DIR) != FR_OK) return 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        unsigned n = shot_index(fi.fname);
        if (n > top) top = n;
    }
    f_closedir(&d);
    return top;
}

/* The header, then the panel from its foot up, as BMP stores it. */
static FRESULT write_image(void) {
    uint8_t head[SHOT_BMP_HEADER];
    shot_bmp_header(head, ACE_PANEL_W, ACE_PANEL_H);
    FRESULT fr = write_all(head, sizeof head);
    uint32_t last_poll = time_us_32();
    for (unsigned y = ACE_PANEL_H; fr == FR_OK && y > 0;) {
        uint8_t *p = s_buf;
        for (unsigned i = 0; i < SHOT_ROWS; i++) {
            display_panel_row(--y, s_px);
            p += shot_bmp_row(s_px, ACE_PANEL_W, p);
        }
        fr = write_all(s_buf, (UINT)(p - s_buf));
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
            g_c1.polls++;
        }
    }
    return fr;
}

static const char *take(void) {
    (void)f_mkdir("/ace");
    (void)f_mkdir(SHOTIO_DIR);
    unsigned n = highest() + 1u;
    if (n > SHOT_LAST) return "SHOT9999 is the last";

    char path[ACE_PATH_MAX], tmp[ACE_PATH_MAX];
    snprintf(path, sizeof path, "%s/SHOT%04u.bmp", SHOTIO_DIR, n);
    snprintf(tmp, sizeof tmp, "%s/SHOT%04u.new", SHOTIO_DIR, n);

    uint32_t t0 = time_us_32();
    FRESULT fr = f_open(&s_f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        log_core1("  shot         : %s: cannot create (FatFs %d)\n", tmp, (int)fr);
        return "Screenshot: cannot create";
    }
    fr = write_image();
    FRESULT fc = f_close(&s_f);
    if (fr == FR_OK) fr = fc;
    if (fr == FR_OK) fr = f_rename(tmp, path);
    uint32_t us = time_us_32() - t0;
    if (fr != FR_OK) {
        (void)f_unlink(tmp);
        log_core1("  shot         : %s: FatFs %d after %lu us\n", path, (int)fr,
                  (unsigned long)us);
        return "Screenshot: card error";
    }
    log_core1("  shot         : %s, %u bytes in %lu us\n", path,
              (unsigned)(SHOT_BMP_HEADER + SHOT_BMP_ROW(ACE_PANEL_W) * ACE_PANEL_H),
              (unsigned long)us);
    snprintf(s_said, sizeof s_said, "SHOT%04u.bmp saved", n);
    return s_said;
}

const char *shotio_take(bool mounted) {
    if (!mounted) {
        if (!sd_present()) return "Screenshot: no card";
        int fr = storage_mount();
        if (fr != 0) {
            log_core1("  shot         : no FAT volume (FatFs %d)\n", fr);
            return "Screenshot: no FAT volume";
        }
    }
    const char *said = take();
    if (!mounted) storage_unmount();
    return said;
}
