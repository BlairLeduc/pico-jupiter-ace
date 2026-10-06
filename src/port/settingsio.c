/* settingsio.c — the settings file on the card (settingsio.h, design.md §10.6). */

#include "settingsio.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "log.h"

static settingsio_state_t s_state = SETTINGSIO_NO_CARD;
static uint32_t s_bytes;
static char s_error[48];
static char s_text[ACE_SETTINGS_FILE_MAX];
static FIL  s_file;

/* The first problem only: the one a user fixes first. */
void settingsio_fail(const char *what, const char *why) {
    log_core1("  settings     : %s: %s\n", what, why);
    if (!s_error[0]) snprintf(s_error, sizeof s_error, "%s: %s", what, why);
}

/* The file's text into s_text: the file, or the temporary one a save
 * left without its rename. FR_NO_FILE if neither is there; FR_DENIED if
 * it is too big to hold. */
static FRESULT read_text(UINT *got, const char **from) {
    *got = 0;
    *from = SETTINGSIO_PATH;
    FRESULT fr = f_open(&s_file, SETTINGSIO_PATH, FA_READ);
    if (fr == FR_NO_FILE) {
        *from = SETTINGSIO_TEMP;
        fr = f_open(&s_file, SETTINGSIO_TEMP, FA_READ);
    }
    if (fr != FR_OK) return fr;
    bool big = f_size(&s_file) > sizeof s_text;
    fr = big ? FR_DENIED : f_read(&s_file, s_text, sizeof s_text, got);
    f_close(&s_file);
    return fr;
}

void settingsio_none(settings_t *out) {
    settings_default(out);
    s_state = SETTINGSIO_NO_CARD;
    s_bytes = 0;
    s_error[0] = 0;
}

void settingsio_load(settings_t *out) {
    settingsio_none(out);

    UINT got = 0;
    const char *from;
    FRESULT fr = read_text(&got, &from);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        s_state = SETTINGSIO_NO_FILE;
        log_core1("  settings     : no %s; defaults\n", SETTINGSIO_PATH);
        return;
    }
    if (fr != FR_OK) {
        s_state = SETTINGSIO_UNREADABLE;
        settingsio_fail("file", fr == FR_DENIED ? "too big" : "cannot read");
        return;
    }

    s_state = SETTINGSIO_READ;
    s_bytes = got;
    unsigned line = 0;
    settings_status_t st = settings_parse(out, s_text, got, &line);
    if (st != SET_OK) {
        char at[16];
        snprintf(at, sizeof at, "line %u", line);
        settingsio_fail(at, settings_status_str(st));
    }
    log_core1("  settings     : %s read, %lu bytes\n", from, (unsigned long)got);
}

settingsio_state_t settingsio_state(void) {
    return s_state;
}

const char *settingsio_state_str(settingsio_state_t st) {
    switch (st) {
    case SETTINGSIO_NO_CARD:    return "no card";
    case SETTINGSIO_NO_FILE:    return "no file";
    case SETTINGSIO_READ:       return "read";
    case SETTINGSIO_UNREADABLE: return "unreadable";
    }
    return "?";
}

uint32_t settingsio_bytes(void) {
    return s_bytes;
}

const char *settingsio_error(void) {
    return s_error;
}

static FRESULT write_all(const char *p, size_t n) {
    UINT put = 0;
    FRESULT fr = f_write(&s_file, p, (UINT)n, &put);
    return fr == FR_OK && put != n ? FR_DENIED : fr;
}

const char *settingsio_save(const settings_t *s) {
    UINT got = 0;
    const char *from;
    FRESULT fr = read_text(&got, &from);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        /* A card without the file gets one: a comment saying what it is,
         * then the lines that differ from the defaults (EL §8.7). */
        static const char head[] = "# pico-ace: what the machine powers on with\n";
        memcpy(s_text, head, sizeof head - 1u);
        got = sizeof head - 1u;
    } else if (fr != FR_OK) {
        return fr == FR_DENIED ? "file too big" : "cannot read";
    }

    const char *text;
    size_t len;
    settings_status_t st = settings_rewrite(s_text, got, s, &text, &len);
    if (st != SET_OK) {
        log_core1("  settings     : not saved: %s\n", settings_status_str(st));
        return settings_status_str(st);
    }

    (void)f_mkdir(SETTINGSIO_DIR);
    fr = f_open(&s_file, SETTINGSIO_TEMP, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = write_all(text, len);
        FRESULT fc = f_close(&s_file);
        if (fr == FR_OK) fr = fc;
    }
    if (fr == FR_OK) {
        (void)f_unlink(SETTINGSIO_PATH);
        fr = f_rename(SETTINGSIO_TEMP, SETTINGSIO_PATH);
    }
    if (fr != FR_OK) {
        log_core1("  settings     : not saved: FatFs error %d\n", (int)fr);
        return "write failed";
    }
    log_core1("  settings     : %s saved, %u bytes\n", SETTINGSIO_PATH, (unsigned)len);
    return NULL;
}
