/* menu.c — the emulator's menu and pause (menu.h, design.md §12). */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "display.h"
#include "handoff.h"
#include "kbd.h"
#include "keymatrix.h"
#include "log.h"
#include "settingsio.h"
#include "snapio.h"
#include "southbridge.h"
#include "storage.h"
#include "tapeio.h"
#include "textpage.h"

/* The keyboard at the live loop's 30 Hz, which also feeds the MCU's 2.5 s
 * bus watchdog (hardware-notes.md §6.1). */
#define POLL_US 33333u

/* Backlight register values step by 16 and clamp to 16-240
 * (hardware-notes.md §6). */
#define BKL_LOWEST 16u

/* The page: a title, the rows, and at the foot the status row and the
 * keys that work here. */
#define ROW_TOP    2
#define ROW_STATUS (TEXT_ROWS - 2)
#define ROW_KEYS   (TEXT_ROWS - 1)

enum { I_TAPE, I_SNAP, I_SETTINGS, I_SAVE, I_RESET, I_COUNT };

/* The Tape page: the deck's two controls, then the files. */
enum { T_EJECT, T_REWIND, T_FIRST };
#define TAPE_ROWS (ROW_STATUS - 1 - (ROW_TOP + 2))

/* The Snapshot page: the slot, chosen with < > on any of its rows, the
 * three things to do with it, then the archive's .ace files (§10.5). */
enum { N_SLOT, N_SAVE, N_LOAD, N_DELETE, N_FIRST };
#define SNAP_ROWS (ROW_STATUS - 1 - (ROW_TOP + N_FIRST + 2))

enum { S_VOLUME, S_PERF, S_COUNT };

static settings_t s_file;     /* what the file says, for the save */
static unsigned   s_slot;     /* the Snapshot page's slot, kept between openings */

static struct {
    ace_t   *m;
    bool     card;
    bool     alt;
    bool     done;
    bool     direct;          /* opened at a page: closing it resumes */
    int      item;
    enum { P_MAIN, P_TAPE, P_SNAP, P_SETTINGS } page;
    char     status[TEXT_COLS + 1];

    unsigned n_tapes;
    int      tape_sel, tape_top;

    int      set_sel;

    bool     used[SNAPIO_SLOTS];
    unsigned n_aces;
    int      snap_sel, snap_top;
} s;

static tapeio_entry_t s_list[ACE_TAPE_LIST_MAX];
static snapio_entry_t s_aces[ACE_SNAP_LIST_MAX];
static uint8_t s_scr[ACE_SCREEN_BYTES];

void menu_init(const settings_t *file) {
    s_file = *file;
}

static void say(const char *fmt, const char *arg) {
    snprintf(s.status, sizeof s.status, fmt, arg);
}

static const char *base(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_main(void) {
    static const char *const items[I_COUNT] = {
        " Tape...                  F1", " Snapshot...              F2", " Settings...",
        " Save settings",
        " Reset                 Alt+R",
    };
    for (int i = 0; i < I_COUNT; i++)
        textpage_line(s_scr, ROW_TOP + i, items[i], i == s.item);

    char line[TEXT_COLS + 1];
    const char *in = tapeio_inserted();
    snprintf(line, sizeof line, " Deck: %.24s", in[0] ? base(in) : "empty");
    textpage_line(s_scr, ROW_TOP + I_COUNT + 1, line, false);
    snprintf(line, sizeof line, " Machine: %s", ace_ram_name(s.m->cfg.ram));
    textpage_line(s_scr, ROW_TOP + I_COUNT + 2, line, false);
}

static void draw_tape(void) {
    char line[TEXT_COLS + 1];
    const char *in = tapeio_inserted();
    if (in[0]) {
        snprintf(line, sizeof line, " Deck: %.16s block %lu", base(in),
                 (unsigned long)tapeio_position());
    } else {
        snprintf(line, sizeof line, " Deck empty: LOAD/SAVE by name");
    }
    textpage_line(s_scr, ROW_TOP, line, false);
    if (!s.card) textpage_line(s_scr, ROW_TOP + 1, " No card", false);
    else if (!s.n_tapes) textpage_line(s_scr, ROW_TOP + 1, " No tapes in /ace/tapes/", false);

    for (int r = 0; r < TAPE_ROWS; r++) {
        int i = s.tape_top + r;
        line[0] = 0;
        if (i == T_EJECT) {
            snprintf(line, sizeof line, " (Empty the deck)");
        } else if (i == T_REWIND) {
            snprintf(line, sizeof line, " (Rewind)");
        } else if (i < T_FIRST + (int)s.n_tapes) {
            const tapeio_entry_t *e = &s_list[i - T_FIRST];
            bool here = strcmp(e->path, in) == 0;
            snprintf(line, sizeof line, "%c%-19.19s %-10.10s", here ? '*' : ' ', base(e->path),
                     e->name);
        }
        textpage_line(s_scr, ROW_TOP + 2 + r, line, i == s.tape_sel);
    }
}

static void draw_snap(void) {
    char line[TEXT_COLS + 1];
    static const char *const what[N_FIRST] = { NULL, " Save state", " Load state", " Delete state" };
    for (int i = 0; i < N_FIRST; i++) {
        if (i == N_SLOT) snprintf(line, sizeof line, " Slot          < %u >  %s", s_slot + 1u,
                                  !s.card ? "" : s.used[s_slot] ? "in use" : "empty");
        else snprintf(line, sizeof line, "%s", what[i]);
        textpage_line(s_scr, ROW_TOP + i, line, i == s.snap_sel);
    }
    int r0 = ROW_TOP + N_FIRST + 1;
    if (!s.card) textpage_line(s_scr, r0, " No card", false);
    else if (!s.n_aces) textpage_line(s_scr, r0, " No .ace files in /ace/snaps/", false);
    else textpage_line(s_scr, r0, " Load an .ace:", false);
    for (int r = 0; r < SNAP_ROWS; r++) {
        int i = s.snap_top + r;
        line[0] = 0;
        if (i < (int)s.n_aces) snprintf(line, sizeof line, " %.30s", base(s_aces[i].path));
        textpage_line(s_scr, r0 + 1 + r, line, N_FIRST + i == s.snap_sel);
    }
}

static void draw_settings(void) {
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof line, " Volume     %u", (unsigned)g_ui.volume);
    textpage_line(s_scr, ROW_TOP + S_VOLUME, line, s.set_sel == S_VOLUME);
    snprintf(line, sizeof line, " Perf line  %s", g_ui.perf_line ? "on" : "off");
    textpage_line(s_scr, ROW_TOP + S_PERF, line, s.set_sel == S_PERF);
    textpage_line(s_scr, ROW_TOP + S_COUNT + 1, " Save settings keeps them", false);
}

static void draw(void) {
    textpage_clear(s_scr);
    textpage_line(s_scr, 0, s.page == P_TAPE ? " Pico-Ace: Tape"
                          : s.page == P_SNAP ? " Pico-Ace: Snapshot"
                          : s.page == P_SETTINGS ? " Pico-Ace: Settings" : " Pico-Ace", true);
    switch (s.page) {
    case P_TAPE:     draw_tape(); break;
    case P_SNAP:     draw_snap(); break;
    case P_SETTINGS: draw_settings(); break;
    default:         draw_main(); break;
    }
    textpage_line(s_scr, ROW_STATUS, s.status, false);
    textpage_line(s_scr, ROW_KEYS, s.page == P_TAPE ? " Enter inserts  Esc back"
                                 : s.page == P_SNAP ? " < > slot  Enter  Esc back"
                                 : s.page == P_SETTINGS ? " < > changes  Esc back"
                                 : " Arrows  Enter  Esc resumes", true);
    display_present(s_scr, display_font(), NULL);
}

/* ---- actions --------------------------------------------------------------- */

static void open_tape(void) {
    s.n_tapes = s.card ? tapeio_list(s_list, ACE_TAPE_LIST_MAX) : 0;
    s.page = P_TAPE;
    s.tape_sel = T_EJECT;
    for (unsigned i = 0; i < s.n_tapes; i++)
        if (strcmp(s_list[i].path, tapeio_inserted()) == 0) s.tape_sel = T_FIRST + (int)i;
    s.tape_top = s.tape_sel >= TAPE_ROWS ? s.tape_sel - TAPE_ROWS + 1 : 0;
}

static void open_snap(void) {
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) s.used[i] = s.card && snapio_exists(i);
    s.n_aces = s.card ? snapio_list_ace(s_aces, ACE_SNAP_LIST_MAX) : 0;
    s.page = P_SNAP;
    s.snap_sel = N_SAVE;
    s.snap_top = 0;
    unsigned used = 0;
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) used += s.used[i];
    log_core1("  snapshot     : page open, slot %u, %u of %u slots in use, %u .ace in "
              SNAPIO_ACE_DIR "\n", s_slot + 1u, used, SNAPIO_SLOTS, s.n_aces);
}

/* The state in the slot, or the .ace chosen: on success the menu closes,
 * and the guest resumes from what was loaded. */
/* A load whose second pass failed has left a machine part old and part
 * new: it is not resumed, but powered on again (snapio.h). */
static bool load_failed_midway(bool changed) {
    if (!changed) return false;
    log_core1("  snapshot     : failed after the machine had changed; powering on again\n");
    g_ui.power_on = true;
    s.done = true;
    return true;
}

static void snap_load_state(void) {
    bool recovered, changed;
    uint32_t us;
    snap_status_t st = snapio_load(s.m, s_slot, &recovered, &changed, &us);
    log_core1("  snapshot     : load slot %u: %s%s, %lu us\n", s_slot + 1u, snapshot_status_str(st),
              recovered ? " (from the unpublished .new)" : "", (unsigned long)us);
    if (st == SNAP_OK) { s.done = true; return; }
    if (load_failed_midway(changed)) return;
    if (st == SNAP_IO && !s.used[s_slot])
        snprintf(s.status, sizeof s.status, " Slot %u is empty", s_slot + 1u);
    else
        say(" Not loaded: %.19s", snapshot_status_str(st));
}

static void snap_load_ace(const char *path) {
    snap_ace_info_t in;
    bool changed;
    uint32_t us;
    snap_ace_status_t st = snapio_load_ace(s.m, path, &in, &changed, &us);
    log_core1("  snapshot     : %s: %s, taken on %s, end $%05lX, PC %04X SP %04X%s, %lu us\n",
              path, snap_ace_status_str(st), snap_ace_taken_on(&in), (unsigned long)in.end, in.pc,
              in.sp, in.repaired ? ", key wait's stack written back" : "", (unsigned long)us);
    if (st == SNAP_ACE_OK) { s.done = true; return; }
    if (load_failed_midway(changed)) return;
    if (st == SNAP_ACE_OTHER_RAM) {
        snprintf(s.status, sizeof s.status, " Needs the %s machine", ace_ram_name(in.needs));
        if (in.ramtop == 0xC000u)
            snprintf(s.status, sizeof s.status, " A 35K file: needs the 51K");
    } else {
        say(" Not loaded: %.19s", snap_ace_status_str(st));
    }
}

static void save_settings(void) {
    if (!s.card) { say(" No card: not saved", ""); return; }
    settings_t out = s_file;
    /* The machine as it runs (EL §10). */
    out.ram = s.m->cfg.ram;
    out.volume = g_ui.volume;
    out.perf_line = g_ui.perf_line;
    settings_card_name(SETTINGS_TAPE_DIR, tapeio_chosen() ? tapeio_inserted() : "",
                       out.boot_tape);
    const char *err = settingsio_save(&out);
    if (!err) s_file = out;
    say(err ? " Not saved: %.20s" : " Settings saved", err);
}

static void key_main(uint8_t c) {
    switch (c) {
    case PICOCALC_KEY_UP:   s.item = (s.item + I_COUNT - 1) % I_COUNT; break;
    case PICOCALC_KEY_DOWN: s.item = (s.item + 1) % I_COUNT; break;
    case PICOCALC_KEY_ENTER:
        s.status[0] = 0;
        switch (s.item) {
        case I_TAPE:     open_tape(); break;
        case I_SNAP:     open_snap(); break;
        case I_SETTINGS: s.page = P_SETTINGS; s.set_sel = S_VOLUME; break;
        case I_SAVE:     save_settings(); break;
        case I_RESET:    g_ui.reset = true; s.done = true; break;
        }
        break;
    case PICOCALC_KEY_ESC:
        s.done = true;
        break;
    }
}

static void key_tape(uint8_t c) {
    int last = T_FIRST + (int)s.n_tapes - 1;
    switch (c) {
    case PICOCALC_KEY_UP:   if (s.tape_sel > 0) s.tape_sel--; break;
    case PICOCALC_KEY_DOWN: if (s.tape_sel < last) s.tape_sel++; break;
    case PICOCALC_KEY_ENTER:
        if (s.tape_sel == T_EJECT) {
            (void)tapeio_insert(NULL);
            say(" Deck empty: LOAD/SAVE by name", "");
        } else if (s.tape_sel == T_REWIND) {
            if (!tapeio_inserted()[0]) { say(" The deck is empty", ""); return; }
            tapeio_rewind();
            say(" Rewound", "");
            return;
        } else {
            const tapeio_entry_t *e = &s_list[s.tape_sel - T_FIRST];
            const char *err = tapeio_insert(e->path);
            if (err) { say(" Not inserted: %.16s", err); return; }
            if (e->name[0])
                snprintf(s.status, sizeof s.status, " In: %s %.10s", e->bytes ? "BLOAD" : "LOAD",
                         e->name);
            else
                say(" In the deck: %.18s", base(e->path));
        }
        s.page = P_MAIN;
        return;
    case PICOCALC_KEY_ESC:
        s.page = P_MAIN;
        return;
    }
    if (s.tape_sel < s.tape_top) s.tape_top = s.tape_sel;
    if (s.tape_sel >= s.tape_top + TAPE_ROWS) s.tape_top = s.tape_sel - TAPE_ROWS + 1;
}

static void key_snap(uint8_t c) {
    int last = N_FIRST + (int)s.n_aces - 1;
    switch (c) {
    case PICOCALC_KEY_UP:   if (s.snap_sel > 0) s.snap_sel--; break;
    case PICOCALC_KEY_DOWN: if (s.snap_sel < last) s.snap_sel++; break;
    case PICOCALC_KEY_LEFT:
    case PICOCALC_KEY_RIGHT:
        if (s.snap_sel < N_FIRST)
            s_slot = (s_slot + (c == PICOCALC_KEY_RIGHT ? 1u : SNAPIO_SLOTS - 1u)) % SNAPIO_SLOTS;
        break;
    case PICOCALC_KEY_ENTER:
        if (!s.card) { say(" No card", ""); break; }
        s.status[0] = 0;
        if (s.snap_sel == N_SAVE) {
            uint32_t us;
            snap_status_t st = snapio_save(s.m, s_slot, &us);
            log_core1("  snapshot     : save slot %u: %s, %lu us\n", s_slot + 1u,
                      snapshot_status_str(st), (unsigned long)us);
            say(st == SNAP_OK ? " Saved" : " Not saved: %.20s", snapshot_status_str(st));
            s.used[s_slot] = snapio_exists(s_slot);
        } else if (s.snap_sel == N_LOAD) {
            snap_load_state();
        } else if (s.snap_sel == N_DELETE) {
            bool gone = snapio_delete(s_slot);
            log_core1("  snapshot     : delete slot %u: %s\n", s_slot + 1u,
                      gone ? "deleted" : "nothing there");
            say(gone ? " Deleted" : " Nothing to delete", "");
            s.used[s_slot] = snapio_exists(s_slot);
        } else if (s.snap_sel >= N_FIRST) {
            snap_load_ace(s_aces[s.snap_sel - N_FIRST].path);
        }
        break;
    case PICOCALC_KEY_ESC:
        s.page = P_MAIN;
        return;
    }
    int i = s.snap_sel - N_FIRST;
    if (i < s.snap_top) s.snap_top = i < 0 ? 0 : i;
    if (i >= s.snap_top + SNAP_ROWS) s.snap_top = i - SNAP_ROWS + 1;
}

static void key_settings(uint8_t c) {
    int d = 0;
    switch (c) {
    case PICOCALC_KEY_UP:    s.set_sel = (s.set_sel + S_COUNT - 1) % S_COUNT; return;
    case PICOCALC_KEY_DOWN:  s.set_sel = (s.set_sel + 1) % S_COUNT; return;
    case PICOCALC_KEY_LEFT:  d = -1; break;
    case PICOCALC_KEY_RIGHT:
    case PICOCALC_KEY_ENTER: d = 1; break;
    case PICOCALC_KEY_ESC:   s.page = P_MAIN; return;
    default: return;
    }
    if (s.set_sel == S_VOLUME) {
        int v = (int)g_ui.volume + d;
        g_ui.volume = (unsigned)(v < 0 ? 0 : v > 8 ? 8 : v);
    } else {
        g_ui.perf_line = !g_ui.perf_line;
    }
}

/* Presses only: releases and the MCU's held reports move nothing. Alt is
 * tracked so that Alt+M closes the menu as it opened it. */
static void keys(void) {
    uint8_t st, c;
    while (!s.done && (kbd_pop(&st, &c) || kbd_pop_uart(&st, &c))) {
        if (c == PICOCALC_KEY_ALT) { s.alt = st != KEY_EV_RELEASED; continue; }
        if (st != KEY_EV_PRESSED) continue;
        if (s.alt && (c == 'm' || c == 'M')) { s.done = true; break; }
        switch (s.page) {
        case P_TAPE:     key_tape(c); break;
        case P_SNAP:     key_snap(c); break;
        case P_SETTINGS: key_settings(c); break;
        default:         key_main(c); break;
        }
        /* A page an F-key opened goes back to the guest, not the main
         * page. */
        if (s.direct && s.page == P_MAIN) s.done = true;
        if (!s.done) draw();
    }
}

void menu_run(ace_t *m, unsigned page, bool alt) {
    memset(&s, 0, sizeof s);
    s.m = m;
    s.alt = alt;

    s.card = storage_mount() == 0;
    if (!s.card) say(" No card: no tapes or snapshots", "");
    const char *t = tapeio_said();
    if (!s.status[0] && t[0]) say("%s", t);
    if (!s.status[0] && settingsio_error()[0]) say(" %.30s", settingsio_error());

    if (page == KM_PAGE_TAPE) {
        s.direct = true;
        open_tape();
    } else if (page == KM_PAGE_SNAPSHOT) {
        s.direct = true;
        open_snap();
    } else if (page != KM_PAGE_MAIN) {
        /* Machine, Layout and About come with later work. */
        say(" Not in this firmware yet", "");
    }

    display_perf("");
    log_core1("  menu         : open at page %u%s\n", page, s.card ? "" : " (no card)");
    draw();

    uint32_t last_poll = time_us_32();
    while (!s.done) {
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
            g_c1.polls++;
            keys();
        }
        log_pump();
        /* Never sleep_us on core 1 (hardware-notes.md §9.7). */
        busy_wait_us_32(500);
    }

    if (s.card) storage_unmount();
    display_invalidate();
    log_core1("  menu         : closed\n");
}

/* ---- pause ------------------------------------------------------------------ */

/* The menu's page for a key, or -1: Alt+M, F1-F5 and F10 (§12). */
static int menu_key(bool alt, uint8_t c) {
    if (alt && (c == 'm' || c == 'M')) return KM_PAGE_MAIN;
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->code == c && (e->flags & KM_MENU) && !(e->flags & KM_ALT)) return e->row;
    }
    return -1;
}

int pause_run(bool *alt_out) {
    display_perf("                Paused");

    /* The level may be the southbridge's own, so it is read, and written
     * back on resume. */
    uint8_t r[2] = { 0, 0 };
    bool read = sb_read(SB_REG_BKL, r) == SB_OK;
    uint8_t level = read ? r[1] : BKL_LOWEST;
    bool dimmed = read && level != BKL_LOWEST && sb_write(SB_REG_BKL, BKL_LOWEST, NULL) == SB_OK;
    log_core1("  pause        : paused, backlight %s\n",
              !read ? "unread, left" : dimmed ? "dimmed" : "already lowest");

    /* It was asked for with Alt held. */
    bool alt = true, done = false;
    int page = -1;
    uint32_t last_poll = time_us_32();
    while (!done) {
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
            g_c1.polls++;
            uint8_t st, c;
            while (!done && (kbd_pop(&st, &c) || kbd_pop_uart(&st, &c))) {
                if (c == PICOCALC_KEY_ALT) { alt = st != KEY_EV_RELEASED; continue; }
                if (st != KEY_EV_PRESSED) continue;
                if (c == PICOCALC_KEY_CTRL || c == PICOCALC_KEY_SHIFT_L ||
                    c == PICOCALC_KEY_SHIFT_R) continue;
                if (alt && (c == 'P' || c == 'p')) continue;
                page = menu_key(alt, c);
                done = true;
            }
        }
        log_pump();
        busy_wait_us_32(500);
    }

    if (dimmed) (void)sb_write(SB_REG_BKL, level, NULL);
    display_perf("");
    log_core1("  pause        : resumed%s\n", page >= 0 ? " into the menu" : "");
    *alt_out = alt;
    return page;
}
