/* keymapio.c — game layouts off the card (keymapio.h, design.md §9.4). */

#include "keymapio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

#include "config.h"
#include "handoff.h"
#include "log.h"

static keylayout_t s_card[ACE_KEYMAP_LAYOUTS];
static unsigned    s_n_card;
static char        s_error[40];
static char        s_text[ACE_KEYMAP_FILE_MAX];
static FIL         s_file;
static char        s_by[ACE_KEYMAP_TAPE_LEN + 5];   /* a file's name, as loaded */

/* Not a directory, nor macOS's "._" shadow of a file. */
static bool is_map(const FILINFO *fi) {
    if (fi->fattrib & AM_DIR) return false;
    if (fi->fname[0] == '.') return false;
    size_t n = strlen(fi->fname);
    return n > 4 && strcasecmp(fi->fname + n - 4, ".map") == 0;
}

static void fail(const char *fname, unsigned line, const char *why) {
    log_core1("  keymaps      : %s/%s line %u: %s; left out\n", KEYMAPIO_DIR, fname, line, why);
    if (s_error[0]) return;
    if (line) snprintf(s_error, sizeof s_error, "%.12s %u: %s", fname, line, why);
    else snprintf(s_error, sizeof s_error, "%.12s: %s", fname, why);
}

/* One file into s_card[s_n_card]. */
static bool load(const char *fname) {
    char path[ACE_PATH_MAX];
    int n = snprintf(path, sizeof path, KEYMAPIO_DIR "/%s", fname);
    if (n < 0 || (size_t)n >= sizeof path) return false;

    if (f_open(&s_file, path, FA_READ) != FR_OK) {
        fail(fname, 0, "cannot open");
        return false;
    }
    UINT got = 0;
    bool big = f_size(&s_file) > sizeof s_text;
    FRESULT fr = big ? FR_OK : f_read(&s_file, s_text, sizeof s_text, &got);
    f_close(&s_file);
    if (big || fr != FR_OK) {
        fail(fname, 0, big ? "too big" : "cannot read");
        return false;
    }

    /* The file's own name, without ".map", if it gives none. */
    char stem[ACE_KEYMAP_NAME_LEN + 1];
    size_t len = strlen(fname) - 4;
    if (len > ACE_KEYMAP_NAME_LEN) len = ACE_KEYMAP_NAME_LEN;
    memcpy(stem, fname, len);
    stem[len] = 0;

    unsigned line = 0;
    keylayout_status_t st = keylayout_parse(&s_card[s_n_card], stem, s_text, got, &line);
    if (st != KL_OK) {
        fail(fname, line, keylayout_status_str(st));
        return false;
    }
    /* STANDARD is the settings file's word for no layout (settings.c),
     * so a layout of that name could be chosen and never saved. */
    if (keymapio_find(s_card[s_n_card].name) >= 0 ||
        strcmp(s_card[s_n_card].name, "STANDARD") == 0) {
        fail(fname, 0, "name taken");
        return false;
    }
    return true;
}

void keymapio_scan(void) {
    char was[ACE_KEYMAP_NAME_LEN + 1] = "";
    if (g_ui.layout) memcpy(was, g_ui.layout->name, sizeof was);
    s_n_card = 0;
    s_error[0] = 0;

    DIR dir;
    static FILINFO fi;
    unsigned room = ACE_KEYMAP_LAYOUTS - (unsigned)keylayout_builtin_len;
    bool open = f_opendir(&dir, KEYMAPIO_DIR) == FR_OK;
    while (open && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!is_map(&fi)) continue;
        if (s_n_card >= room) {
            fail(fi.fname, 0, "too many layouts");
            break;
        }
        if (load(fi.fname)) {
            const keylayout_t *l = &s_card[s_n_card++];
            log_core1("  keymaps      : %s: \"%s\", %u binding(s), %u tape(s)\n", fi.fname,
                   l->name, l->n, l->n_tapes);
        }
    }
    if (open) f_closedir(&dir);

    int i = was[0] ? keymapio_find(was) : -1;
    g_ui.layout = i >= 0 ? keymapio_get((unsigned)i) : NULL;
    if (!g_ui.layout) s_by[0] = 0;
}

void keymapio_choose(const keylayout_t *l) {
    g_ui.layout = l;
    s_by[0] = 0;
    log_core1("  keymaps      : \"%s\" chosen\n", l ? l->name : "STANDARD");
}

/* Loading any other file leaves the choice alone: a game's loader may
 * fetch its next part under another name. */
void keymapio_file_loaded(const char *path) {
    const keylayout_t *l = keymapio_for_file(path);
    if (!l) return;
    const char *b = strrchr(path, '/');
    snprintf(s_by, sizeof s_by, "%s", b ? b + 1 : path);
    g_ui.layout = l;
    log_core1("  keymaps      : loading %s chose \"%s\"\n", s_by, l->name);
}

const char *keymapio_chosen_by(void) {
    return s_by;
}

unsigned keymapio_count(void) {
    return (unsigned)keylayout_builtin_len + s_n_card;
}

const keylayout_t *keymapio_get(unsigned i) {
    if (i < keylayout_builtin_len) return &keylayout_builtin[i];
    i -= (unsigned)keylayout_builtin_len;
    return i < s_n_card ? &s_card[i] : NULL;
}

int keymapio_find(const char *name) {
    for (unsigned i = 0; i < keymapio_count(); i++) {
        if (strcmp(keymapio_get(i)->name, name) == 0) return (int)i;
    }
    return -1;
}

const keylayout_t *keymapio_for_file(const char *path) {
    for (unsigned i = 0; i < keymapio_count(); i++) {
        if (keylayout_for_file(keymapio_get(i), path)) return keymapio_get(i);
    }
    return NULL;
}

const char *keymapio_error(void) {
    return s_error;
}
