/* menu.c — the emulator's menu and pause (menu.h, design.md §12).
 *
 * pico-atom's menu, page for page and row for row, so that the two
 * emulators read as one family: the same items in the same order, the
 * same function keys, the same rows on each page, and Esc going back a
 * page, or to the guest from the main page or a page a key opened. The
 * Ace has no disc, so there is no Discs page, and F2 is nothing. The
 * text is in the Ace's own character set, which has lower case.
 */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ace_rom.h"
#include "board.h"
#include "display.h"
#include "handoff.h"
#include "kbd.h"
#include "keymapio.h"
#include "keymatrix.h"
#include "log.h"
#include "pico_ace_version.h"
#include "settingsio.h"
#include "shotio.h"
#include "snapio.h"
#include "southbridge.h"
#include "status.h"
#include "storage.h"
#include "tapeio.h"
#include "textpage.h"

/* The keyboard at the live loop's 30 Hz, which also feeds the MCU's 2.5 s
 * bus watchdog (hardware-notes.md §6.1). */
#define POLL_US 33333u

/* The MCU refreshes its battery reading every 20 s (hardware-notes.md
 * §6); reading it more often than this would show nothing new. */
#define BAT_POLL_US 5000000u

/* Backlight register values step by 16 and clamp to 16-240
 * (hardware-notes.md §6). */
#define BKL_STEP   16u
#define BKL_LOWEST 16u

/* The page: a title, the rows from row 2, and at the foot the status
 * row and the keys that work here. */
#define ROW_TOP    2
#define ROW_STATUS (TEXT_ROWS - 2)
#define ROW_KEYS   (TEXT_ROWS - 1)

/* pico-atom's items, less Discs. The function keys open the first four
 * (keymatrix.h's KM_PAGE_*). */
enum { I_TAPES, I_SNAPS, I_SETUP, I_MACHINE, I_RESET, I_SAVE, I_ABOUT, I_COUNT };

/* The Tapes page: the deck's controls, then the files. Play is the
 * signal's (design.md §10.4), for a loader that never calls the ROM.
 * pico-atom's Record is not here: the Ace's recorder starts a block at
 * the ROM's save cue (cassette.h), so every SAVE records, and a recorder
 * started by hand would take nothing. */
enum { T_EJECT, T_PLAY, T_REWIND, T_NEW, T_FIRST };
#define TAPE_ROWS (ROW_STATUS - 1 - (ROW_TOP + 1))

/* The Snapshots page: the slot, chosen with < > on any of its rows, the
 * three things to do with it, every slot's state, then the archive's
 * .ace files (§10.5), which the cursor moves on to. */
enum { N_SLOT, N_SAVE, N_LOAD, N_DELETE, N_FIRST };
#define SNAP_ACE_TOP (ROW_TOP + N_FIRST + 1 + (int)SNAPIO_SLOTS + 1)
#define SNAP_ROWS    (ROW_STATUS - 1 - (SNAP_ACE_TOP + 1))

/* The Setup page, as pico-atom's has it, less the Atom's display rows:
 * the lines, the backlight, the volume and the keys, then fast tape. */
enum { D_STATUS, D_PERF, D_BACKLIGHT, D_VOLUME, D_KEYS, D_FAST, D_COUNT };

/* The Machine page: the RAM size staged, and the power-on that applies
 * it. */
enum { M_RAM, M_APPLY, M_COUNT };

static settings_t s_file;     /* what the file says, for the save */
static unsigned   s_slot;     /* the Snapshots page's slot, kept between openings */

static struct {
    ace_t   *m;
    bool     card;
    bool     alt;
    bool     done;
    bool     direct;          /* opened at a page: closing it resumes */
    int      item;
    enum { P_MAIN, P_TAPES, P_SNAPS, P_SETUP, P_MACHINE, P_ABOUT, P_HELP } page;
    char     status[TEXT_COLS + 1];
    int      battery;         /* SB_REG_BAT's byte, -1 if unread */

    unsigned n_tapes;
    int      tape_sel, tape_top;

    bool     used[SNAPIO_SLOTS];
    unsigned n_aces;
    int      snap_sel, snap_top;

    int      setup_sel;

    int       machine_sel;
    ace_ram_t st_ram;         /* staged: applied only by a power-on */

    int      sb_ver;          /* the About page's, read as it opens */
    int      temp_c;
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

static const char *on_off(bool v) {
    return v ? "on" : "off";
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_main(void) {
    static const char *const items[I_COUNT] = {
        " Tapes...", " Snapshots...", " Setup...", " Machine...", " Reset",
        " Save settings", " About...",
    };
    for (int i = 0; i < I_COUNT; i++)
        textpage_line(s_scr, ROW_TOP + i, items[i], i == s.item);

    /* The deck, with the cassette's word and place while it holds the
     * tape (status.h), and the machine. */
    char line[TEXT_COLS + 1], deck[12] = "";
    ace_status_t st;
    ace_status(s.m, &st);
    if (st.deck != STATUS_DECK_IDLE) {
        static const char *const word[] = { "", "Stop", "Play", "End", "Rec", "Full" };
        snprintf(deck, sizeof deck, " %s %u%%", word[st.deck], (unsigned)st.percent);
    }
    const char *in = tapeio_inserted();
    snprintf(line, sizeof line, " Tape in: %.*s%s", deck[0] ? 12 : 21,
             in[0] ? base(in) : "none", deck);
    textpage_line(s_scr, ROW_TOP + I_COUNT + 1, line, false);
    snprintf(line, sizeof line, " Machine: %s  Keys: %.10s", ace_ram_name(s.m->cfg.ram),
             g_ui.layout ? g_ui.layout->name : "standard");
    textpage_line(s_scr, ROW_TOP + I_COUNT + 2, line, false);
}

static void draw_tapes(void) {
    char line[TEXT_COLS + 1];
    const cassette_t *cas = &s.m->cas;
    textpage_line(s_scr, ROW_TOP, !s.card ? " No card"
                                : s.n_tapes ? " File                First file"
                                : " No tapes in /ace/tapes/", false);
    for (int r = 0; r < TAPE_ROWS; r++) {
        int i = s.tape_top + r;
        line[0] = 0;
        if (i == T_EJECT) {
            snprintf(line, sizeof line, " (Eject)");
        } else if (i == T_PLAY) {
            snprintf(line, sizeof line, " (%s)", g_ui.fast_tape ? "Play: fast tape is on"
                                               : cas->playing ? "Stop" : "Play");
        } else if (i == T_REWIND) {
            snprintf(line, sizeof line, " (Rewind)");
        } else if (i == T_NEW) {
            snprintf(line, sizeof line, " (New tape)");
        } else if (i < T_FIRST + (int)s.n_tapes) {
            const tapeio_entry_t *e = &s_list[i - T_FIRST];
            bool here = strcmp(e->path, tapeio_inserted()) == 0;
            snprintf(line, sizeof line, "%c%-19.19s %-10.10s", here ? '*' : ' ', base(e->path),
                     e->name[0] ? e->name : "(empty)");
        }
        textpage_line(s_scr, ROW_TOP + 1 + r, line, i == s.tape_sel);
    }
}

static void draw_snaps(void) {
    char line[TEXT_COLS + 1];
    static const char *const what[N_FIRST] = { NULL, " Save", " Load", " Delete" };
    for (int i = 0; i < N_FIRST; i++) {
        if (i == N_SLOT) snprintf(line, sizeof line, " Slot            < %u >", s_slot + 1u);
        else snprintf(line, sizeof line, "%s", what[i]);
        textpage_line(s_scr, ROW_TOP + i, line, i == s.snap_sel);
    }
    /* Every slot's state, the chosen one marked. */
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) {
        snprintf(line, sizeof line, "%cSlot %u: %s", i == s_slot ? '*' : ' ', i + 1u,
                 !s.card ? "no card" : s.used[i] ? "saved" : "empty");
        textpage_line(s_scr, ROW_TOP + N_FIRST + 1 + (int)i, line, false);
    }
    textpage_line(s_scr, SNAP_ACE_TOP, !s.card ? "" : s.n_aces ? " Load an .ace file:"
                                     : " No .ace files in /ace/snaps/", false);
    for (int r = 0; r < SNAP_ROWS; r++) {
        int i = s.snap_top + r;
        line[0] = 0;
        if (i < (int)s.n_aces) snprintf(line, sizeof line, " %.30s", base(s_aces[i].path));
        textpage_line(s_scr, SNAP_ACE_TOP + 1 + r, line, N_FIRST + i == s.snap_sel);
    }
}

static void draw_setup(void) {
    char line[TEXT_COLS + 1];
    for (int i = 0; i < D_COUNT; i++) {
        switch (i) {
        case D_STATUS:
            snprintf(line, sizeof line, " Status line     < %s >", on_off(g_ui.status));
            break;
        case D_PERF:
            snprintf(line, sizeof line, " Perf line       < %s >", on_off(g_ui.perf_line));
            break;
        case D_BACKLIGHT:
            snprintf(line, sizeof line, " Backlight       < %u >", g_ui.backlight);
            break;
        case D_VOLUME:
            snprintf(line, sizeof line, " Volume          < %u >", g_ui.volume);
            break;
        case D_KEYS:
            /* A card layout's name is cut to keep the column. */
            snprintf(line, sizeof line, " Keys            < %.11s >",
                     g_ui.layout ? g_ui.layout->name : "standard");
            break;
        case D_FAST:
            snprintf(line, sizeof line, " Fast tape       < %s >", on_off(g_ui.fast_tape));
            break;
        }
        textpage_line(s_scr, ROW_TOP + i, line, i == s.setup_sel);
    }
}

static void draw_machine(void) {
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof line, "%cRAM             < %s >", s.st_ram != s.m->cfg.ram ? '*' : ' ',
             ace_ram_name(s.st_ram));
    textpage_line(s_scr, ROW_TOP + M_RAM, line, s.machine_sel == M_RAM);
    textpage_line(s_scr, ROW_TOP + M_APPLY, " (Apply and restart)", s.machine_sel == M_APPLY);
    textpage_line(s_scr, ROW_TOP + M_COUNT + 1, " - Program in memory is lost on", false);
    textpage_line(s_scr, ROW_TOP + M_COUNT + 2, "   restart.", false);
    textpage_line(s_scr, ROW_TOP + M_COUNT + 3, " - The 3K is the Ace as sold; the", false);
    textpage_line(s_scr, ROW_TOP + M_COUNT + 4, "   19K and 51K have a RAM pack.", false);
}

static void draw_about(void) {
    char line[TEXT_COLS + 1];
    const board_info_t *b = &g_board;
    snprintf(line, sizeof line, " Pico-Ace %.22s", PICO_ACE_VERSION);
    textpage_line(s_scr, 2, line, false);
    snprintf(line, sizeof line, " Board %.24s", b->sdk_board);
    textpage_line(s_scr, 3, line, false);
    char sb[4] = "??";
    if (s.sb_ver >= 0) snprintf(sb, sizeof sb, "%02X", (unsigned)(s.sb_ver & 0xFF));
    /* Whole degrees and uncalibrated (hardware-notes.md §8.1), clamped
     * so the row is never more than TEXT_COLS. */
    int t = s.temp_c < -99 ? -99 : s.temp_c > 999 ? 999 : s.temp_c;
    snprintf(line, sizeof line, " %s rev %X %u MHz SB %s %dC", b->chip ? b->chip : "?",
             b->chip_version & 0xFu, (unsigned)((b->clk_sys_hz / 1000000u) % 1000u), sb, t);
    textpage_line(s_scr, 4, line, false);
    snprintf(line, sizeof line, " Machine %s", ace_ram_name(s.m->cfg.ram));
    textpage_line(s_scr, 5, line, false);

    /* The ROM, in the firmware and checked at build time (§10.2), as
     * pico-atom lists each of its sockets. */
    snprintf(line, sizeof line, " $0000 ace.rom      OK %.8s", ACE_ROM_SHA1);
    textpage_line(s_scr, 7, line, false);

    const char *err = settingsio_error();
    snprintf(line, sizeof line, " Settings %.22s",
             err[0] ? err : settingsio_state_str(settingsio_state()));
    textpage_line(s_scr, 9, line, false);
}

/* The keys the emulator takes for itself (§9.2, §12), one a row. */
static void draw_help(void) {
    static const char *const keys[][2] = {
        { "F1",     "Tapes" },
        { "F3",     "Snapshots" },
        { "F4",     "Setup" },
        { "F5",     "Machine" },
        { "F10",    "About" },
        { "F6",     "Screenshot" },
        { "Alt+M",  "Menu" },
        { "Alt+H",  "These keys" },
        { "Alt+P",  "Pause" },
        { "Alt+K",  "Reset" },
        { "Esc",    "Break" },
        { "Ctrl",   "Symbol shift" },
        { "Alt+L",  "Caps lock" },
        { "Alt+G",  "Graphics" },
        { "Alt+V",  "Inverse video" },
        { "Alt+X",  "Delete line" },
    };
    char line[TEXT_COLS + 1];
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        snprintf(line, sizeof line, " %-11s %s", keys[i][0], keys[i][1]);
        textpage_line(s_scr, ROW_TOP + (int)i, line, false);
    }
}

/* The title row's right end: the charge, and Chg in place of Bat while
 * it charges (bit 7, hardware-notes.md §6). Nothing if it could not be
 * read. */
static void draw_battery(void) {
    if (s.battery < 0) return;
    unsigned pct = (unsigned)s.battery & 0x7Fu;
    char text[12];
    snprintf(text, sizeof text, "%s %u%% ", s.battery & 0x80 ? "Chg" : "Bat",
             pct > 100u ? 100u : pct);
    textpage_put(s_scr, 0, TEXT_COLS - (int)strlen(text), text, true);
}

static bool read_battery(void) {
    uint8_t r[2];
    int was = s.battery;
    s.battery = sb_read(SB_REG_BAT, r) == SB_OK ? r[1] : -1;
    return s.battery != was;
}

static void draw(void) {
    static const char *const title[] = {
        [P_MAIN] = " Pico-Ace",               [P_TAPES] = " Pico-Ace: Tapes",
        [P_SNAPS] = " Pico-Ace: Snapshots",   [P_SETUP] = " Pico-Ace: Setup",
        [P_MACHINE] = " Pico-Ace: Machine",   [P_ABOUT] = " Pico-Ace: About",
        [P_HELP] = " Pico-Ace: Keys",
    };
    static const char *const keys[] = {
        [P_MAIN] = " Arrows  Enter  Esc resumes", [P_TAPES] = " Enter inserts  Esc back",
        [P_SNAPS] = " < > slot  Enter  Esc back", [P_SETUP] = " < > changes  Esc back",
        [P_MACHINE] = " < > stages  Enter  Esc back", [P_ABOUT] = " Esc back",
        [P_HELP] = " Esc resumes",
    };
    textpage_clear(s_scr);
    textpage_line(s_scr, 0, title[s.page], true);
    draw_battery();
    switch (s.page) {
    case P_TAPES:   draw_tapes(); break;
    case P_SNAPS:   draw_snaps(); break;
    case P_SETUP:   draw_setup(); break;
    case P_MACHINE: draw_machine(); break;
    case P_ABOUT:   draw_about(); break;
    case P_HELP:    draw_help(); break;
    default:        draw_main(); break;
    }
    textpage_line(s_scr, ROW_STATUS, s.status, false);
    textpage_line(s_scr, ROW_KEYS, keys[s.page], true);
    display_present(s_scr, display_font(), NULL);
}

/* ---- opening the pages ------------------------------------------------------ */

static void open_tapes(void) {
    s.n_tapes = s.card ? tapeio_list(s_list, ACE_TAPE_LIST_MAX) : 0;
    s.page = P_TAPES;
    s.tape_sel = T_EJECT;
    for (unsigned i = 0; i < s.n_tapes; i++)
        if (strcmp(s_list[i].path, tapeio_inserted()) == 0) s.tape_sel = T_FIRST + (int)i;
    s.tape_top = s.tape_sel >= TAPE_ROWS ? s.tape_sel - TAPE_ROWS + 1 : 0;
}

static void refresh_slots(void) {
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) s.used[i] = s.card && snapio_exists(i);
}

static void open_snaps(void) {
    refresh_slots();
    s.n_aces = s.card ? snapio_list_ace(s_aces, ACE_SNAP_LIST_MAX) : 0;
    s.page = P_SNAPS;
    s.snap_sel = N_SAVE;
    s.snap_top = 0;
    unsigned used = 0;
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) used += s.used[i];
    log_core1("  snapshot     : page open, slot %u, %u of %u slots in use, %u .ace in "
              SNAPIO_ACE_DIR "\n", s_slot + 1u, used, SNAPIO_SLOTS, s.n_aces);
}

static void open_machine(void) {
    s.page = P_MACHINE;
    s.machine_sel = M_RAM;
    s.st_ram = s.m->cfg.ram;
}

static void open_about(void) {
    uint8_t r[2];
    s.page = P_ABOUT;
    s.sb_ver = sb_read(SB_REG_VER, r) == SB_OK ? r[1] : -1;
    s.temp_c = board_temp_c();
    log_core1("  about        : %s, %s rev %u, id %s, %lu MHz, southbridge %d, die %d C, "
              "ROM %s, %s, settings %s\n", PICO_ACE_VERSION, g_board.chip,
              (unsigned)g_board.chip_version, g_board.unique_id,
              (unsigned long)(g_board.clk_sys_hz / 1000000u), s.sb_ver, s.temp_c, ACE_ROM_SHA1,
              ace_ram_name(s.m->cfg.ram),
              settingsio_error()[0] ? settingsio_error() : settingsio_state_str(settingsio_state()));
}

/* ---- actions --------------------------------------------------------------- */

/* A load whose second pass failed has left a machine part old and part
 * new: it is not resumed, but powered on again (snapio.h). */
static bool load_failed_midway(bool changed) {
    if (!changed) return false;
    log_core1("  snapshot     : failed after the machine had changed; powering on again\n");
    g_ui.power_on = true;
    s.done = true;
    return true;
}

/* The state in the slot, or the .ace chosen: on success the menu closes,
 * and the guest resumes from what was loaded. */
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
    if (st == SNAP_ACE_OK) {
        keymapio_file_loaded(path);   /* a layout may name it (§9.4) */
        s.done = true;
        return;
    }
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
    out.status = g_ui.status;
    out.fast_tape = g_ui.fast_tape;
    /* 0 is a backlight never read, which keeps the file's (settings.h). */
    out.backlight = g_ui.backlight;
    settings_card_name(SETTINGS_TAPE_DIR, tapeio_chosen() ? tapeio_inserted() : "",
                       out.boot_tape);
    /* A layout a loaded file chose is not the user's choice, so the file
     * keeps what it had (§9.4). */
    if (!keymapio_chosen_by()[0])
        snprintf(out.layout, sizeof out.layout, "%s", g_ui.layout ? g_ui.layout->name : "");
    const char *err = settingsio_save(&out);
    if (!err) s_file = out;
    say(err ? " Not saved: %.20s" : " Settings saved", err);
}

/* Apply (§12): the staged machine powered on, with the deck, the layout
 * and the settings as they are. A recording not on the card yet would be
 * lost, so it is refused until the recording has been written. */
static void apply_machine(void) {
    uint32_t from, to;
    if (ace_cassette_unsaved(s.m, &from, &to)) {
        say(" A recording is not saved yet", "");
        return;
    }
    ace_ram_t was = s.m->cfg.ram;
    s.m->cfg.ram = s.st_ram;
    ace_power_on(s.m);
    log_core1("  machine      : %s powered on as the %s\n", ace_ram_name(was),
              ace_ram_name(s.m->cfg.ram));
    s.done = true;             /* straight into the new machine */
}

static void open_item(void) {
    s.status[0] = 0;
    switch (s.item) {
    case I_TAPES:   open_tapes(); break;
    case I_SNAPS:   open_snaps(); break;
    case I_SETUP:   s.page = P_SETUP; s.setup_sel = D_STATUS; break;
    case I_MACHINE: open_machine(); break;
    case I_RESET:   g_ui.reset = true; s.done = true; break;
    case I_SAVE:    save_settings(); break;
    case I_ABOUT:   open_about(); break;
    }
}

/* ---- keys ------------------------------------------------------------------- */

static void key_main(uint8_t c) {
    switch (c) {
    case PICOCALC_KEY_UP:    s.item = (s.item + I_COUNT - 1) % I_COUNT; break;
    case PICOCALC_KEY_DOWN:  s.item = (s.item + 1) % I_COUNT; break;
    case PICOCALC_KEY_ENTER: open_item(); break;
    case PICOCALC_KEY_ESC:   s.done = true; break;
    }
}

static void key_tapes(uint8_t c) {
    int last = T_FIRST + (int)s.n_tapes - 1;
    switch (c) {
    case PICOCALC_KEY_UP:   if (s.tape_sel > 0) s.tape_sel--; break;
    case PICOCALC_KEY_DOWN: if (s.tape_sel < last) s.tape_sel++; break;
    case PICOCALC_KEY_ENTER:
        if (s.tape_sel == T_EJECT) {
            (void)tapeio_insert(s.m, NULL);
            say(" Tape ejected", "");
        } else if (s.tape_sel == T_PLAY) {
            bool on = !s.m->cas.playing;
            const char *err = tapeio_play(s.m, on);
            if (err) { say(" Not played: %.19s", err); return; }
            if (!on) { say(" Stopped", ""); return; }
            /* Back to the guest, whose loader is waiting for it. */
            s.done = true;
            return;
        } else if (s.tape_sel == T_REWIND) {
            if (!tapeio_inserted()[0]) { say(" The deck is empty", ""); return; }
            tapeio_rewind(s.m);
            say(" Rewound", "");
            return;
        } else if (s.tape_sel == T_NEW) {
            if (!s.card) { say(" No card", ""); return; }
            const char *err = tapeio_new(s.m);
            if (err) { say(" No new tape: %.18s", err); return; }
            say(" In: %.12s, SAVE onto it", base(tapeio_inserted()));
            s.n_tapes = tapeio_list(s_list, ACE_TAPE_LIST_MAX);
            return;
        } else {
            const tapeio_entry_t *e = &s_list[s.tape_sel - T_FIRST];
            const char *err = tapeio_insert(s.m, e->path);
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

static void key_snaps(uint8_t c) {
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
            say(" Saving...", "");
            draw();
            uint32_t us;
            snap_status_t st = snapio_save(s.m, s_slot, &us);
            log_core1("  snapshot     : save slot %u: %s, %lu us\n", s_slot + 1u,
                      snapshot_status_str(st), (unsigned long)us);
            say(st == SNAP_OK ? " Saved" : " Not saved: %.20s", snapshot_status_str(st));
            refresh_slots();
        } else if (s.snap_sel == N_LOAD) {
            say(" Loading...", "");
            draw();
            s.status[0] = 0;
            snap_load_state();
        } else if (s.snap_sel == N_DELETE) {
            bool gone = snapio_delete(s_slot);
            log_core1("  snapshot     : delete slot %u: %s\n", s_slot + 1u,
                      gone ? "deleted" : "nothing there");
            say(gone ? " Deleted" : " Nothing to delete", "");
            refresh_slots();
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

/* Standard, then keymapio's list, round and round, as pico-atom's Keys
 * row goes. */
static void cycle_keys(int dir) {
    int n = (int)keymapio_count() + 1, at = 0;
    for (unsigned i = 0; i < keymapio_count(); i++)
        if (keymapio_get(i) == g_ui.layout) at = (int)i + 1;
    at = (at + n + dir) % n;
    keymapio_choose(at ? keymapio_get((unsigned)(at - 1)) : NULL);
}

static void set_backlight(int dir) {
    int v = (int)g_ui.backlight + dir;
    v = v < 1 ? 1 : v > 15 ? 15 : v;
    g_ui.backlight = (unsigned)v;
    (void)sb_write(SB_REG_BKL, (uint8_t)(v * (int)BKL_STEP), NULL);
}

static void key_setup(uint8_t c) {
    int dir = 0;
    switch (c) {
    case PICOCALC_KEY_UP:    s.setup_sel = (s.setup_sel + D_COUNT - 1) % D_COUNT; return;
    case PICOCALC_KEY_DOWN:  s.setup_sel = (s.setup_sel + 1) % D_COUNT; return;
    case PICOCALC_KEY_LEFT:  dir = -1; break;
    case PICOCALC_KEY_RIGHT: dir = 1; break;
    case PICOCALC_KEY_ENTER: dir = 0; break;
    case PICOCALC_KEY_ESC:   s.page = P_MAIN; return;
    default: return;
    }
    switch (s.setup_sel) {
    /* Core 1 draws the lines; they show once the menu closes. */
    case D_STATUS: g_ui.status = !g_ui.status; break;
    case D_PERF:   g_ui.perf_line = !g_ui.perf_line; break;
    case D_BACKLIGHT:
        if (dir) set_backlight(dir);
        break;
    case D_VOLUME:
        if (dir) {
            int v = (int)g_ui.volume + dir;
            g_ui.volume = (unsigned)(v < 0 ? 0 : v > 8 ? 8 : v);
        }
        break;
    case D_KEYS:
        if (dir) cycle_keys(dir);
        break;
    case D_FAST:
        g_ui.fast_tape = !g_ui.fast_tape;
        tapeio_mode(s.m);
        log_core1("  menu         : fast tape %s\n", on_off(g_ui.fast_tape));
        break;
    }
}

static void key_machine(uint8_t c) {
    switch (c) {
    case PICOCALC_KEY_UP:
    case PICOCALC_KEY_DOWN: s.machine_sel = (s.machine_sel + 1) % M_COUNT; break;
    case PICOCALC_KEY_LEFT:
    case PICOCALC_KEY_RIGHT:
        if (s.machine_sel == M_RAM) {
            const int n = (int)ACE_RAM_51K + 1;
            s.st_ram = (ace_ram_t)(((int)s.st_ram + (c == PICOCALC_KEY_RIGHT ? 1 : n - 1)) % n);
            say(s.st_ram != s.m->cfg.ram ? " Apply restarts: program lost" : "", "");
        }
        break;
    case PICOCALC_KEY_ENTER:
        if (s.machine_sel == M_APPLY) {
            if (s.st_ram != s.m->cfg.ram) {
                say(" Restarting...", "");
                draw();
                apply_machine();
            } else {
                say(" Nothing to apply", "");
            }
        }
        break;
    case PICOCALC_KEY_ESC:
        /* Nothing changes until Apply. */
        say(s.st_ram != s.m->cfg.ram ? " Not applied" : "", "");
        s.page = P_MAIN;
        break;
    }
}

/* F6 takes one screenshot a press. The MCU's auto-repeat arrives as more
 * presses, and the SD write polls the keyboard, so it rearms only on the
 * release (hardware-notes.md §6.2), which is F1's if Shift went first.
 * True when this event is F6 going down afresh. */
static bool s_shot_down;

static bool shot_press(uint8_t st, uint8_t c) {
    if (keymap_picocalc_canonical(c) != keymap_picocalc_canonical(PICOCALC_KEY_F6)) return false;
    if (st == KEY_EV_RELEASED) s_shot_down = false;
    if (st != KEY_EV_PRESSED || c != PICOCALC_KEY_F6 || s_shot_down) return false;
    s_shot_down = true;
    return true;
}

/* Presses only: releases and the MCU's held reports move nothing. Alt is
 * tracked so that Alt+M closes the menu as it opened it. */
static void keys(void) {
    uint8_t st, c;
    while (!s.done && (kbd_pop(&st, &c) || kbd_pop_uart(&st, &c))) {
        if (c == PICOCALC_KEY_ALT) { s.alt = st != KEY_EV_RELEASED; continue; }
        bool shoot = shot_press(st, c);
        if (st != KEY_EV_PRESSED) continue;
        if (s.alt && (c == 'm' || c == 'M')) { s.done = true; break; }
        /* F6 on any page: the page as it is, then its status row says
         * how it went. Its repeats do nothing. */
        if (c == PICOCALC_KEY_F6) {
            if (shoot) {
                say(" %s", shotio_take(s.card));
                draw();
            }
            continue;
        }
        switch (s.page) {
        case P_TAPES:   key_tapes(c); break;
        case P_SNAPS:   key_snaps(c); break;
        case P_SETUP:   key_setup(c); break;
        case P_MACHINE: key_machine(c); break;
        case P_ABOUT:
        case P_HELP:
            if (c == PICOCALC_KEY_ESC || c == PICOCALC_KEY_ENTER) s.page = P_MAIN;
            break;
        default:        key_main(c); break;
        }
        /* A page a key opened goes back to the guest, not the main page. */
        if (s.direct && s.page == P_MAIN) s.done = true;
        if (!s.done) draw();
    }
}

void menu_run(ace_t *m, unsigned page, bool alt) {
    memset(&s, 0, sizeof s);
    s.m = m;
    s_shot_down = false;
    s.alt = alt;       /* Alt+M or Alt+H has it held; the function keys do not */

    s.card = storage_mount() == 0;
    if (!s.card) say(" No card: no tapes or snapshots", "");
    /* The card's layouts afresh: a file may have been added or edited
     * on a computer since (keymapio.h). */
    if (s.card) keymapio_scan();
    /* A layout a file chose says so (§9.4), then the tape's last word,
     * then the first problem with the files. */
    if (!s.status[0] && g_ui.layout && keymapio_chosen_by()[0])
        say(" Keys chosen by %.16s", keymapio_chosen_by());
    const char *t = tapeio_said();
    if (!s.status[0] && t[0]) say("%s", t);
    if (!s.status[0] && settingsio_error()[0]) say(" %.30s", settingsio_error());
    if (!s.status[0] && keymapio_error()[0]) say(" %.30s", keymapio_error());

    /* Opened by a function key or Alt+H: that page, keeping what the
     * status row says, and closing it closes the menu. */
    s.direct = page != KM_PAGE_MAIN;
    char said[sizeof s.status];
    memcpy(said, s.status, sizeof said);
    switch (page) {
    case KM_PAGE_TAPE:     s.item = I_TAPES; open_item(); break;
    case KM_PAGE_SNAPSHOT: s.item = I_SNAPS; open_item(); break;
    case KM_PAGE_SETUP:    s.item = I_SETUP; open_item(); break;
    case KM_PAGE_MACHINE:  s.item = I_MACHINE; open_item(); break;
    case KM_PAGE_ABOUT:    s.item = I_ABOUT; open_item(); break;
    case KM_PAGE_HELP:     s.page = P_HELP; break;
    default: break;
    }
    if (!s.status[0]) memcpy(s.status, said, sizeof said);

    s.battery = -1;
    (void)read_battery();

    /* The lines describe the running machine, and the menu can change
     * what they say, so they are hidden while it is open; each is drawn
     * only when its text changes, so the first present after it closes
     * draws them afresh. */
    display_perf("");
    display_status("");

    log_core1("  menu         : open at page %u%s\n", page, s.card ? "" : " (no card)");
    draw();

    uint32_t last_poll = time_us_32(), last_bat = last_poll;
    while (!s.done) {
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
            g_c1.polls++;
            keys();
            if (!s.done && last_poll - last_bat >= BAT_POLL_US) {
                last_bat = last_poll;
                bool changed = read_battery();
                if (s.page == P_ABOUT) {
                    int tc = board_temp_c();
                    changed |= tc != s.temp_c;
                    s.temp_c = tc;
                }
                if (changed) draw();
            }
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

/* The menu's page for a key, or -1: Alt+M, Alt+H and the function keys
 * (§12), from the same table the guest's keys come from. */
static int menu_key(bool alt, uint8_t c) {
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (!(e->flags & KM_MENU)) continue;
        bool layer = (e->flags & KM_ALT) != 0;
        if (layer && alt && (c == e->code || c == e->code + ('a' - 'A'))) return e->row;
        if (!layer && c == e->code) return e->row;
    }
    return -1;
}

int pause_run(bool *alt_out) {
    /* The status line says so, whether or not it is on, as pico-atom's
     * does. */
    char line[ACE_TEXT_COLS + 1];
    status_paused_format(line);
    display_status(line);

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
    s_shot_down = false;
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
                bool shoot = shot_press(st, c);
                if (st != KEY_EV_PRESSED) continue;
                if (c == PICOCALC_KEY_CTRL || c == PICOCALC_KEY_SHIFT_L ||
                    c == PICOCALC_KEY_SHIFT_R) continue;
                if (alt && (c == 'P' || c == 'p')) continue;
                /* F6 takes the paused frame and stays paused, saying how
                 * it went where Paused was; its repeats neither take
                 * another nor resume. */
                if (c == PICOCALC_KEY_F6) {
                    if (shoot) {
                        char said[ACE_TEXT_COLS + 1];
                        snprintf(said, sizeof said, "Paused: %s", shotio_take(false));
                        display_status(said);
                    }
                    continue;
                }
                page = menu_key(alt, c);
                done = true;
            }
        }
        log_pump();
        busy_wait_us_32(500);
    }

    if (dimmed) (void)sb_write(SB_REG_BKL, level, NULL);
    display_status("");   /* the next present draws the line as it is */
    log_core1("  pause        : resumed%s\n", page >= 0 ? " into the menu" : "");
    *alt_out = alt;
    return page;
}
