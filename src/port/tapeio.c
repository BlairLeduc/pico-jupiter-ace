/* tapeio.c — .tap files serving the tape trap (tapeio.h, design.md §10.3). */

#include "tapeio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "log.h"
#include "storage.h"

volatile tapeio_stats_t g_tape_stats;

/* The deck. `user` is a tape put in by the menu or boot_tape, which a
 * save appends to; one LOAD found by name is not. */
static char     s_path[ACE_PATH_MAX];
static bool     s_user;
static uint32_t s_pos;        /* byte offset of the next block         */
static uint32_t s_index;      /* its index from the start, for its flag */
static bool     s_wrapped;    /* rewound by a load since the last data  */

/* SAVE's header named this file, for the data block after it. */
static char     s_save_path[ACE_PATH_MAX];

static char     s_said[40];
static FIL      s_f, s_g;
static uint8_t  s_buf[ACE_TAPE_CHUNK];

static void say(const char *fmt, const char *arg) {
    snprintf(s_said, sizeof s_said, fmt, arg);
}

const char *tapeio_said(void) {
    static char out[sizeof s_said];
    memcpy(out, s_said, sizeof out);
    s_said[0] = 0;
    return out;
}

static const char *base(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* ---- the deck ------------------------------------------------------------- */

static void set_deck(const char *path, bool user) {
    snprintf(s_path, sizeof s_path, "%s", path ? path : "");
    s_user = user && s_path[0];
    s_pos = s_index = 0;
    s_wrapped = false;
}

/* The file, or the .new a save left without its rename. */
static FRESULT open_read(const char *path) {
    FRESULT fr = f_open(&s_f, path, FA_READ);
    if (fr == FR_NO_FILE) {
        char tmp[ACE_PATH_MAX + 4];
        snprintf(tmp, sizeof tmp, "%s.new", path);
        fr = f_open(&s_f, tmp, FA_READ);
    }
    return fr;
}

const char *tapeio_insert(const char *path) {
    if (!path || !path[0]) {
        set_deck(NULL, false);
        return NULL;
    }
    if (open_read(path) != FR_OK) {
        set_deck(NULL, false);
        return "cannot open";
    }
    f_close(&s_f);
    set_deck(path, true);
    log_core1("  tape         : %s in the deck\n", path);
    return NULL;
}

const char *tapeio_inserted(void) { return s_path; }
bool tapeio_chosen(void) { return s_user; }
uint32_t tapeio_position(void) { return s_index; }

void tapeio_rewind(void) {
    s_pos = s_index = 0;
    s_wrapped = false;
}

/* The ten-character name at addr, as a file in TAPEIO_DIR: trailing
 * spaces gone, and anything FAT will not take as '_'. False for a name
 * that is all spaces. */
static bool name_path(const ace_t *m, uint16_t addr, char out[ACE_PATH_MAX]) {
    char name[TAPE_NAME_LEN + 1];
    unsigned n = 0;
    for (unsigned i = 0; i < TAPE_NAME_LEN; i++) {
        char c = (char)(ace_peek(m, (uint16_t)(addr + i)) & 0x7Fu);
        if (c < 0x20 || c == 0x7F || strchr("\"*/:<>?\\|", c)) c = '_';
        name[i] = c;
        if (c != ' ') n = i + 1u;
    }
    name[n] = 0;
    if (!n) return false;
    snprintf(out, ACE_PATH_MAX, "%s/%s.tap", TAPEIO_DIR, name);
    return true;
}

/* ---- load ------------------------------------------------------------------- */

/* With the deck empty and no file of that name: the first tape in
 * TAPEIO_DIR whose first header carries the name, compared as the ROM
 * compares ($1AA9), byte for byte with its padding. Archive files are
 * seldom named after the program they hold. */
static bool find_by_header(const ace_t *m, char out[ACE_PATH_MAX]) {
    uint8_t want[TAPE_NAME_LEN];
    for (unsigned i = 0; i < TAPE_NAME_LEN; i++)
        want[i] = ace_peek(m, (uint16_t)(TAPE_HEADER_ASKED + 1u + i));
    DIR d;
    FILINFO fi;
    bool found = false;
    if (f_opendir(&d, TAPEIO_DIR) != FR_OK) return false;
    while (!found && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t len = strlen(fi.fname);
        if ((fi.fattrib & (AM_DIR | AM_HID)) || strncmp(fi.fname, "._", 2) == 0 ||
            len < 5 || strcasecmp(fi.fname + len - 4, ".tap") != 0) continue;
        snprintf(out, ACE_PATH_MAX, "%s/%s", TAPEIO_DIR, fi.fname);
        uint8_t h[2 + 1 + TAPE_NAME_LEN];
        UINT got = 0;
        if (f_open(&s_f, out, FA_READ) != FR_OK) continue;
        found = f_read(&s_f, h, sizeof h, &got) == FR_OK && got == sizeof h &&
                h[0] == TAPE_HEADER_LEN + 1u && h[1] == 0 &&
                memcmp(h + 3, want, TAPE_NAME_LEN) == 0;
        f_close(&s_f);
    }
    f_closedir(&d);
    return found;
}

static void decline(ace_t *m, const char *why) {
    ace_tape_decline(m);
    g_tape_stats.declined++;
    log_core1("  tape         : declined: %s\n", why);
}

static void serve_load(ace_t *m, const tape_t *t) {
    /* LOAD's header read names a file: that file, if the card has it. */
    char named[ACE_PATH_MAX];
    if (t->flag == TAPE_FLAG_HEADER && t->ret == TAPE_LOAD_HEADER_RET &&
        name_path(m, TAPE_HEADER_ASKED + 1u, named) && strcmp(named, s_path) != 0) {
        FRESULT fr = open_read(named);
        if (fr == FR_OK) {
            f_close(&s_f);
            set_deck(named, false);
            log_core1("  tape         : %s found by name\n", named);
        } else if (!s_path[0] && find_by_header(m, named)) {
            set_deck(named, false);
            log_core1("  tape         : %s found by its header\n", named);
        } else {
            log_core1("  tape         : no %s (FatFs %d)\n", named, (int)fr);
        }
    }
    if (!s_path[0]) {
        decline(m, "no tape, and no file by that name");
        return;
    }
    if (open_read(s_path) != FR_OK) {
        g_tape_stats.errors++;
        say(" Cannot open %.24s", base(s_path));
        decline(m, "cannot open the tape");
        return;
    }

    uint8_t hdr[2];
    UINT got = 0;
    FSIZE_t size = f_size(&s_f);
    if (s_pos + 2u > size) {
        if (t->flag == TAPE_FLAG_HEADER && !s_wrapped) {
            s_pos = s_index = 0;
            s_wrapped = true;
            log_core1("  tape         : end of %s; rewound\n", s_path);
        } else {
            f_close(&s_f);
            say(" End of tape %.24s", base(s_path));
            decline(m, "end of the tape");
            return;
        }
    }
    if (f_lseek(&s_f, s_pos) != FR_OK || f_read(&s_f, hdr, 2, &got) != FR_OK || got != 2) {
        f_close(&s_f);
        g_tape_stats.errors++;
        say(" Cannot read %.24s", base(s_path));
        decline(m, "cannot read the tape");
        return;
    }
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8);
    uint8_t flag = tape_block_flag(s_index);
    bool wanted = ace_tape_load_begin(m, flag);
    uint32_t left = len;
    while (left && ace_tape_load_wants(m)) {
        UINT want = left < sizeof s_buf ? (UINT)left : (UINT)sizeof s_buf;
        if (f_read(&s_f, s_buf, want, &got) != FR_OK || got == 0) break;
        ace_tape_load_data(m, s_buf, got);
        left -= got;
    }
    f_close(&s_f);
    ace_tape_load_end(m);

    s_pos += 2u + len;
    s_index++;
    if (wanted && flag == TAPE_FLAG_DATA) s_wrapped = false;
    g_tape_stats.loads++;
    g_tape_stats.bytes = len;
    log_core1("  tape         : block %lu of %s, %lu bytes, %s%s\n",
              (unsigned long)(s_index - 1u), base(s_path), (unsigned long)len,
              flag == TAPE_FLAG_HEADER ? "header" : "data",
              wanted ? "" : ", not the kind asked for");
}

/* ---- save ------------------------------------------------------------------- */

static FRESULT write_all(FIL *f, const void *p, UINT n) {
    UINT put = 0;
    FRESULT fr = f_write(f, p, n, &put);
    return fr == FR_OK && put != n ? FR_DENIED : fr;
}

/* path's blocks, then this one, into path.new; then the rename. */
static FRESULT append(const ace_t *m, const tape_t *t, const char *path) {
    char tmp[ACE_PATH_MAX + 4];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    (void)f_mkdir("/ace");
    (void)f_mkdir(TAPEIO_DIR);

    /* A save cut off between the unlink and the rename left only the
     * .new, which open_read plays; make it the tape before it is
     * overwritten. Beside the tape, a .new may be partial: the tape wins. */
    FILINFO fi;
    if (f_stat(path, &fi) == FR_NO_FILE && f_stat(tmp, &fi) == FR_OK) {
        FRESULT fr = f_rename(tmp, path);
        if (fr != FR_OK) return fr;
    }

    FRESULT fr = f_open(&s_g, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) return fr;
    bool had = open_read(path) == FR_OK;
    UINT got = 0;
    while (fr == FR_OK && had) {
        fr = f_read(&s_f, s_buf, sizeof s_buf, &got);
        if (fr != FR_OK || got == 0) break;
        fr = write_all(&s_g, s_buf, got);
    }
    if (had) f_close(&s_f);

    uint32_t n = (uint32_t)t->len + 1u;
    uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
    if (fr == FR_OK) fr = write_all(&s_g, len, 2);
    uint8_t sum = 0;
    for (uint32_t done = 0; fr == FR_OK && done < t->len;) {
        UINT k = 0;
        while (k < sizeof s_buf && done < t->len) {
            uint8_t b = ace_peek(m, (uint16_t)(t->addr + done++));
            sum ^= b;
            s_buf[k++] = b;
        }
        fr = write_all(&s_g, s_buf, k);
    }
    if (fr == FR_OK) fr = write_all(&s_g, &sum, 1);
    FRESULT fc = f_close(&s_g);
    if (fr == FR_OK) fr = fc;
    if (fr != FR_OK) {
        (void)f_unlink(tmp);
        return fr;
    }
    (void)f_unlink(path);
    return f_rename(tmp, path);
}

static void serve_save(ace_t *m, const tape_t *t) {
    const char *path = s_path;
    if (!s_user) {
        /* SAVE's header names the file; its data block follows it. */
        if (t->flag == TAPE_FLAG_HEADER && t->ret == TAPE_SAVE_HEADER_RET &&
            !name_path(m, (uint16_t)(t->addr + 1u), s_save_path))
            s_save_path[0] = 0;
        path = s_save_path;
    }
    if (!path[0]) {
        say(" No tape in the deck to save on", "");
        decline(m, "nowhere to save");
        return;
    }
    FRESULT fr = append(m, t, path);
    if (fr != FR_OK) {
        g_tape_stats.errors++;
        say(" Not saved: card %s", fr == FR_DENIED ? "full" : "error");
        log_core1("  tape         : not saved to %s: FatFs %d\n", path, (int)fr);
        decline(m, "the card refused the save");
        return;
    }
    g_tape_stats.saves++;
    g_tape_stats.bytes = t->len;
    log_core1("  tape         : %u-byte %s appended to %s\n", (unsigned)t->len,
              t->flag == TAPE_FLAG_HEADER ? "header" : "data block", path);
    if (t->flag != TAPE_FLAG_HEADER) {
        say(" Saved to %.22s", base(path));
        /* One header, one data block: a stray block is not added. */
        if (!s_user) s_save_path[0] = 0;
    }
    ace_tape_save_end(m);
}

void tapeio_serve(ace_t *m, uint32_t *us) {
    uint32_t t0 = time_us_32();
    const tape_t *t = ace_tape_pending(m);
    if (!t) { *us = 0; return; }
    int err = storage_mount();
    if (err != 0) {
        say(" No card: tape not served", "");
        decline(m, "no card");
    } else {
        if (t->op == TAPE_SAVE) serve_save(m, t);
        else serve_load(m, t);
        storage_unmount();
    }
    *us = time_us_32() - t0;
    g_tape_stats.last_us = *us;
    if (*us > g_tape_stats.max_us) g_tape_stats.max_us = *us;
}

/* ---- the menu's list -------------------------------------------------------- */

unsigned tapeio_list(tapeio_entry_t *out, unsigned max) {
    DIR d;
    FILINFO fi;
    unsigned n = 0;
    if (f_opendir(&d, TAPEIO_DIR) != FR_OK) return 0;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        /* Directories, and the "._" files macOS writes beside each file
         * it copies (its AppleDouble metadata, not a tape). */
        if ((fi.fattrib & (AM_DIR | AM_HID)) || strncmp(fi.fname, "._", 2) == 0) continue;
        size_t len = strlen(fi.fname);
        if (len < 5 || strcasecmp(fi.fname + len - 4, ".tap") != 0) continue;
        tapeio_entry_t *e = &out[n];
        snprintf(e->path, sizeof e->path, "%s/%s", TAPEIO_DIR, fi.fname);
        e->size = (uint32_t)fi.fsize;
        e->name[0] = 0;
        e->bytes = false;
        /* The first block, if it is a header: its type and name. */
        uint8_t h[2 + TAPE_HEADER_LEN];
        UINT got = 0;
        if (f_open(&s_f, e->path, FA_READ) == FR_OK) {
            if (f_read(&s_f, h, sizeof h, &got) == FR_OK && got == sizeof h &&
                h[0] == TAPE_HEADER_LEN + 1u && h[1] == 0) {
                e->bytes = h[2] != 0;
                unsigned k = 0;
                for (unsigned i = 0; i < TAPE_NAME_LEN; i++) {
                    char c = (char)(h[3 + i] & 0x7Fu);
                    e->name[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
                    if (c != ' ') k = i + 1u;
                }
                e->name[k] = 0;
            }
            f_close(&s_f);
        }
        n++;
    }
    f_closedir(&d);
    return n;
}
