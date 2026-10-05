/* tapeio.c — .tap files serving the tape trap (tapeio.h, design.md §10.3). */

#include "tapeio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "handoff.h"
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

/* The signal (design.md §10.4): the deck's tape whole, which the
 * cassette plays, and where a recording goes. An image of no path is the
 * recorder's scratch, for a save to a file that is not the one loaded. */
static uint8_t  s_img[ACE_TAPE_IMAGE_MAX];
static char     s_img_path[ACE_PATH_MAX];
static char     s_rec_path[ACE_PATH_MAX];

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

/* The cassette holds this file's image, or the scratch. A machine
 * powered on again has an empty cassette whatever was in it. */
static bool img_in(const ace_t *m) {
    return m && m->cas.loaded && m->cas.img == s_img;
}

static void cassette_out(ace_t *m) {
    if (img_in(m)) ace_cassette_eject(m);
    s_img_path[0] = 0;
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

const char *tapeio_insert(ace_t *m, const char *path) {
    cassette_out(m);
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

uint32_t tapeio_position(const ace_t *m) {
    if (g_ui.fast_tape) return s_index;
    return img_in(m) && s_img_path[0] ? m->cas.index : 0;
}

void tapeio_rewind(ace_t *m) {
    s_pos = s_index = 0;
    s_wrapped = false;
    if (img_in(m) && s_img_path[0]) ace_cassette_rewind(m);
}

/* The tape at `path` whole into the cassette, unless it is there. */
static const char *image_load(ace_t *m, const char *path) {
    if (img_in(m) && strcmp(s_img_path, path) == 0) return NULL;
    cassette_out(m);
    if (open_read(path) != FR_OK) return "cannot open";
    FSIZE_t size = f_size(&s_f);
    UINT got = 0;
    FRESULT fr = size > sizeof s_img ? FR_DENIED : f_read(&s_f, s_img, (UINT)size, &got);
    f_close(&s_f);
    if (size > sizeof s_img) return "longer than 64K";
    if (fr != FR_OK || got != size) return "cannot read";
    ace_cassette_insert(m, s_img, got, sizeof s_img);
    snprintf(s_img_path, sizeof s_img_path, "%s", path);
    log_core1("  tape         : %s, %lu bytes, in the cassette\n", path, (unsigned long)got);
    return NULL;
}

const char *tapeio_play(ace_t *m, bool on) {
    if (!on) {
        ace_cassette_play(m, false);
        return NULL;
    }
    if (g_ui.fast_tape) return "fast tape is on";
    if (!s_path[0]) return "the deck is empty";
    const char *err = image_load(m, s_path);
    if (err) return err;
    if (m->cas.ended) return "at the end: rewind";
    ace_cassette_play(m, true);
    log_core1("  tape         : playing %s from block %lu by hand\n", s_path,
              (unsigned long)m->cas.index);
    return NULL;
}

void tapeio_mode(ace_t *m) {
    if (g_ui.fast_tape) cassette_out(m);
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

/* LOAD's header read names a file: that file, if the card has it; with
 * the deck empty, the first tape whose first header has the name. */
static void find_load(const ace_t *m, const tape_t *t) {
    /* Two buffers: the search leaves the last file it looked at in its
     * own, and the log names the file asked for. */
    char named[ACE_PATH_MAX], found[ACE_PATH_MAX];
    char hex[2 * 12 + 1];
    for (unsigned i = 0; i < 12u; i++)
        snprintf(hex + 2u * i, 3, "%02X", ace_peek(m, (uint16_t)(TAPE_HEADER_ASKED + i)));
    log_core1("  tape         : asked at $%04X: %s, ret $%04X, block $%04X+%u\n",
              TAPE_HEADER_ASKED, hex, t->ret, t->addr, (unsigned)t->len);
    if (t->flag == TAPE_FLAG_HEADER && t->ret == TAPE_LOAD_HEADER_RET &&
        name_path(m, TAPE_HEADER_ASKED + 1u, named) && strcmp(named, s_path) != 0) {
        FRESULT fr = open_read(named);
        if (fr == FR_OK) {
            f_close(&s_f);
            set_deck(named, false);
            log_core1("  tape         : %s found by name\n", named);
        } else if (!s_path[0] && find_by_header(m, found)) {
            set_deck(found, false);
            log_core1("  tape         : %s found by its header\n", found);
        } else {
            log_core1("  tape         : no %s (FatFs %d)\n", named, (int)fr);
        }
    }
}

static void serve_load(ace_t *m, const tape_t *t) {
    find_load(m, t);
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

/* path's blocks into path.new, open in s_g for what follows. */
static FRESULT append_begin(const char *path, char tmp[ACE_PATH_MAX + 4]) {
    snprintf(tmp, ACE_PATH_MAX + 4, "%s.new", path);
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
    if (fr != FR_OK) f_close(&s_g);
    return fr;
}

/* Close path.new, and rename it over path if all went well. */
static FRESULT append_end(const char *path, const char *tmp, FRESULT fr) {
    FRESULT fc = f_close(&s_g);
    if (fr == FR_OK) fr = fc;
    if (fr != FR_OK) {
        (void)f_unlink(tmp);
        return fr;
    }
    (void)f_unlink(path);
    return f_rename(tmp, path);
}

/* path's blocks, then this one from guest memory, into path.new; then
 * the rename. */
static FRESULT append(const ace_t *m, const tape_t *t, const char *path) {
    char tmp[ACE_PATH_MAX + 4];
    FRESULT fr = append_begin(path, tmp);
    if (fr != FR_OK) return fr;
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
    return append_end(path, tmp, fr);
}

/* The same with blocks already in .tap form: what the recorder took. */
static FRESULT append_bytes(const char *path, const uint8_t *p, uint32_t n) {
    char tmp[ACE_PATH_MAX + 4];
    FRESULT fr = append_begin(path, tmp);
    if (fr != FR_OK) return fr;
    return append_end(path, tmp, write_all(&s_g, p, (UINT)n));
}

/* The deck's tape if the user put it there; otherwise the file SAVE's
 * header names, for the header and the data block after it. */
static const char *save_path(const ace_t *m, const tape_t *t) {
    if (s_user) return s_path;
    if (t->flag == TAPE_FLAG_HEADER && t->ret == TAPE_SAVE_HEADER_RET &&
        !name_path(m, (uint16_t)(t->addr + 1u), s_save_path))
        s_save_path[0] = 0;
    return s_save_path;
}

static void serve_save(ace_t *m, const tape_t *t) {
    const char *path = save_path(m, t);
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

/* ---- the signal (design.md §10.4) ------------------------------------------ *
 * The trap still stalls the CPU at each block routine, and the port
 * finds the tape as it does for the trap, but then declines: the ROM's
 * routine runs, and the cassette follows its cue (cassette.h). */

static void signal_load(ace_t *m, const tape_t *t) {
    find_load(m, t);
    if (!s_path[0]) {
        cassette_out(m);
        decline(m, "no tape, and no file by that name");
        return;
    }
    const char *err = image_load(m, s_path);
    if (err) {
        g_tape_stats.errors++;
        say(" Tape not played: %.18s", err);
        log_core1("  tape         : %s: %s\n", s_path, err);
        cassette_out(m);
        decline(m, "the tape cannot be played");
        return;
    }
    /* At the end of the tape a LOAD rewinds once, as the trap does. */
    if (t->flag == TAPE_FLAG_HEADER) {
        if (m->cas.ended && !s_wrapped) {
            ace_cassette_rewind(m);
            s_wrapped = true;
            log_core1("  tape         : end of %s; rewound\n", s_path);
        }
    } else {
        s_wrapped = false;
    }
    g_tape_stats.loads++;
    log_core1("  tape         : %s plays from block %lu for the ROM's %s read\n",
              base(s_path), (unsigned long)m->cas.index,
              t->flag == TAPE_FLAG_HEADER ? "header" : "data");
    ace_tape_decline(m);
}

static void signal_save(ace_t *m, const tape_t *t) {
    const char *path = save_path(m, t);
    if (!path[0]) {
        say(" No tape in the deck to save on", "");
        decline(m, "nowhere to save");
        return;
    }
    if (s_user) {
        /* Recorded onto the deck's own tape, at its end. */
        const char *err = image_load(m, path);
        if (err) {
            g_tape_stats.errors++;
            say(" Not recorded: %.20s", err);
            decline(m, "the tape cannot be recorded on");
            return;
        }
    } else if (!img_in(m) || s_img_path[0]) {
        cassette_out(m);
        ace_cassette_insert(m, s_img, 0, sizeof s_img);
    }
    snprintf(s_rec_path, sizeof s_rec_path, "%s", path);
    ace_cassette_record(m, true);
    log_core1("  tape         : recording the ROM's %s for %s\n",
              t->flag == TAPE_FLAG_HEADER ? "header" : "data block", path);
    ace_tape_decline(m);
}

/* What the recorder took since the last park, appended to its file. */
static void signal_flush(ace_t *m) {
    uint32_t from, to;
    if (!ace_cassette_unsaved(m, &from, &to)) return;
    bool deck = s_img_path[0] && strcmp(s_img_path, s_rec_path) == 0;
    FRESULT fr = s_rec_path[0] ? append_bytes(s_rec_path, s_img + from, to - from)
                               : FR_INVALID_NAME;
    if (fr == FR_OK) {
        g_tape_stats.saves++;
        g_tape_stats.bytes = to - from;
        say(" Saved to %.22s", base(s_rec_path));
        log_core1("  tape         : %lu recorded bytes appended to %s\n",
                  (unsigned long)(to - from), s_rec_path);
        /* A short recording in the log, so that a run over the UART can
         * check it off the board (design.md §15.2 M13). */
        for (uint32_t i = from; i < to && i - from < 256u; i += 32u) {
            char hex[32 * 2 + 1];
            uint32_t k = 0;
            for (; k < 32u && i + k < to; k++)
                snprintf(hex + 2u * k, 3, "%02X", s_img[i + k]);
            log_core1("  tape rec     : %s\n", hex);
        }
    } else {
        g_tape_stats.errors++;
        say(" Not saved: card %s", fr == FR_DENIED ? "full" : "error");
        log_core1("  tape         : recording not saved to %s: FatFs %d\n", s_rec_path, (int)fr);
    }
    /* Kept in the deck's image only if it is on the card too. */
    ace_cassette_saved(m, fr == FR_OK || !deck);
    ace_cassette_record(m, false);
}

void tapeio_serve(ace_t *m, uint32_t *us) {
    uint32_t t0 = time_us_32();
    const tape_t *t = ace_tape_pending(m);
    uint32_t from, to;
    bool unsaved = ace_cassette_unsaved(m, &from, &to);
    if (!t && !unsaved) { *us = 0; return; }
    int err = storage_mount();
    if (err != 0) {
        say(" No card: tape not served", "");
        if (unsaved) {
            g_tape_stats.errors++;
            ace_cassette_saved(m, false);
            ace_cassette_record(m, false);
        }
        if (t) decline(m, "no card");
    } else {
        if (unsaved) signal_flush(m);
        if (t && g_ui.fast_tape) {
            if (t->op == TAPE_SAVE) serve_save(m, t);
            else serve_load(m, t);
        } else if (t) {
            if (t->op == TAPE_SAVE) signal_save(m, t);
            else signal_load(m, t);
        }
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
