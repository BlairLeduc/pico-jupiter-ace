/* test_keymap.c — the keymap table and the held-key set (design.md §9).
 * Needs no ROM run; test_keyboard checks the same table against the ROM.
 */

#include <stdio.h>
#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "keymatrix.h"
#include "test_util.h"

static ace_t       m;
static keymatrix_t k;

static void fresh(void) {
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    ace_init(&m, &cfg);
    keymatrix_init(&k);
}

static bool cell_down(uint8_t row, uint8_t col) {
    return (m.keys[row] >> col) & 1u;
}

static bool shift_down(void) { return cell_down(AK_ROW_MODS, AK_COL_SHIFT); }
static bool sym_down(void)   { return cell_down(AK_ROW_MODS, AK_COL_SYM); }

static bool matrix_empty(void) {
    for (unsigned r = 0; r < ACE_KEY_ROWS; r++)
        if (m.keys[r]) return false;
    return true;
}

static const keymap_t *entry_for(uint8_t code, bool alt) {
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->code == code && !!(e->flags & KM_ALT) == alt) return e;
    }
    return NULL;
}

static void press(uint8_t code)   { keymatrix_event(&k, KEY_EV_PRESSED, code); }
static void release(uint8_t code) { keymatrix_event(&k, KEY_EV_RELEASED, code); }
static void fields(int n) { for (int i = 0; i < n; i++) keymatrix_field(&k, &m); }

int main(void) {
    /* ---- table invariants (§9.2, §9.3) -------------------------------- */
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (!(e->flags & KM_NOCELL)) {
            CHECK(e->row < ACE_KEY_ROWS && e->col < ACE_KEY_COLS,
                  "code 0x%02X: cell (%u,%u) off the matrix", e->code, e->row, e->col);
            /* SHIFT and SYMBOL SHIFT are flags, never an entry's cell. */
            CHECK(!(e->row == AK_ROW_MODS && e->col <= AK_COL_SYM),
                  "code 0x%02X bound to a modifier's cell", e->code);
            CHECK(!((e->flags & KM_SHIFT) && (e->flags & KM_SYM)),
                  "code 0x%02X wants SHIFT and SYMBOL SHIFT", e->code);
        }
        if (e->flags & KM_MENU)
            CHECK(e->row <= KM_PAGE_ABOUT, "code 0x%02X opens page %u", e->code, e->row);
        /* The MCU keeps Alt+, . Space and B for itself, and Alt+I is its
         * Insert (hardware-notes.md §6.3): a binding there never fires. */
        if (e->flags & KM_ALT) {
            CHECK(strchr(",. BI", e->code) == NULL, "Alt+'%c' is consumed by the MCU",
                  e->code);
            CHECK(e->code >= 'A' && e->code <= 'Z',
                  "Alt+0x%02X: Alt letters arrive in capitals", e->code);
        }
        CHECK(e->code != PICOCALC_KEY_ALT && e->code != PICOCALC_KEY_CTRL &&
                  e->code != PICOCALC_KEY_SHIFT_L && e->code != PICOCALC_KEY_SHIFT_R,
              "a modifier is bound as a key");
        for (size_t j = i + 1; j < keymap_picocalc_len; j++) {
            const keymap_t *f = &keymap_picocalc[j];
            CHECK(!(e->code == f->code && (e->flags & KM_ALT) == (f->flags & KM_ALT)),
                  "code 0x%02X bound twice", e->code);
        }
    }
    /* Every character the PicoCalc types reaches the Ace (EL §7.2). */
    for (unsigned c = 0x20; c < 0x7F; c++)
        CHECK(entry_for((uint8_t)c, false) != NULL, "no entry for '%c'", c);

    /* BREAK is a plain key, since Shift+Space never arrives (§9.2). */
    {
        const keymap_t *esc = entry_for(PICOCALC_KEY_ESC, false);
        CHECK(esc && esc->row == 7 && esc->col == 0 && (esc->flags & KM_SHIFT),
              "Esc is SHIFT+SPACE");
    }

    /* Canonical identity is idempotent, and maps both halves of a key
     * to one physical key. */
    for (unsigned c = 0; c < 256; c++) {
        uint8_t once = keymap_picocalc_canonical((uint8_t)c);
        CHECK(keymap_picocalc_canonical(once) == once, "canonical(0x%02X) not stable", c);
    }
    CHECK(keymap_picocalc_canonical('A') == 'a', "A is a");
    CHECK(keymap_picocalc_canonical('!') == '1', "! is 1");
    CHECK(keymap_picocalc_canonical(':') == ';', ": is ;");

    /* ---- text as PicoCalc events: the UART's path (§9.1) ------------- */
    {
        picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
        CHECK(keymap_picocalc_text('a', ev) == 2 && ev[0].state == KEY_EV_PRESSED &&
                  ev[0].code == 'a' && ev[1].state == KEY_EV_RELEASED && ev[1].code == 'a',
              "a is a press and a release");
        CHECK(keymap_picocalc_text('!', ev) == 4 && ev[0].code == PICOCALC_KEY_SHIFT_L &&
                  ev[1].code == '!' && ev[2].code == '!' && ev[2].state == KEY_EV_RELEASED &&
                  ev[3].code == PICOCALC_KEY_SHIFT_L && ev[3].state == KEY_EV_RELEASED,
              "! is inside Shift, as the PicoCalc types it");
        CHECK(keymap_picocalc_text('A', ev) == 4 && ev[0].code == PICOCALC_KEY_SHIFT_L,
              "A is inside Shift");
        CHECK(keymap_picocalc_text('`', ev) == 2, "` is a key of its own");
        CHECK(keymap_picocalc_text('\r', ev) == 2 && ev[0].code == PICOCALC_KEY_ENTER,
              "CR is Enter");
        CHECK(keymap_picocalc_text('\n', ev) == 2 && ev[0].code == PICOCALC_KEY_ENTER,
              "LF is Enter");
        CHECK(keymap_picocalc_text(0x7Fu, ev) == 2 && ev[0].code == PICOCALC_KEY_BACKSPACE,
              "DEL is Backspace");
        CHECK(keymap_picocalc_text(0x1Bu, ev) == 2 && ev[0].code == PICOCALC_KEY_ESC,
              "ESC is Esc");
        CHECK(keymap_picocalc_text(0x09u, ev) == 4 && ev[0].code == PICOCALC_KEY_CTRL &&
                  ev[1].code == 'i' && ev[3].code == PICOCALC_KEY_CTRL,
              "^I is Ctrl+i");
        CHECK(keymap_picocalc_text(0x00u, ev) == 0 && keymap_picocalc_text(0x80u, ev) == 0 &&
                  keymap_picocalc_text(0x1Cu, ev) == 0,
              "bytes no key sends give no events");
        /* Every printable byte's events reach a binding in the table. */
        for (unsigned c = 0x20; c < 0x7F; c++) {
            unsigned n = keymap_picocalc_text((uint8_t)c, ev);
            CHECK(n >= 2 && entry_for(ev[n == 4 ? 1 : 0].code, false),
                  "'%c' sends a code with no binding", c);
        }
    }

    /* ---- the held set: pacing (§9.1) ---------------------------------- */
    {
        /* Press and release in one poll: the key is down for exactly
         * ACE_KEY_MIN_FIELDS fields. */
        fresh();
        press('a');
        release('a');
        unsigned down = 0;
        for (int f = 0; f < 20; f++) {
            fields(1);
            if (cell_down(1, 0)) down++;
        }
        CHECK(down == ACE_KEY_MIN_FIELDS, "held %u fields, want %u", down,
              (unsigned)ACE_KEY_MIN_FIELDS);
        CHECK(!shift_down() && !sym_down(), "a is unshifted");

        /* Two keys in one poll are serialised with the gap between. */
        fresh();
        press('a');
        release('a');
        press('b');
        release('b');
        int a_last = -1, b_first = -1;
        for (int f = 0; f < 40; f++) {
            fields(1);
            CHECK(!(cell_down(1, 0) && cell_down(7, 3)), "a and b overlap at field %d", f);
            if (cell_down(1, 0)) a_last = f;
            if (cell_down(7, 3) && b_first < 0) b_first = f;
        }
        CHECK(b_first - a_last - 1 == (int)ACE_KEY_GAP_FIELDS,
              "gap of %d fields, want %u", b_first - a_last - 1,
              (unsigned)ACE_KEY_GAP_FIELDS);

        /* A key held by a human is held here: auto-repeat presses do not
         * release it, and it goes up only when released. */
        fresh();
        press('j');
        for (int f = 0; f < 30; f++) {
            if (f % 6 == 0) press('j');
            fields(1);
            CHECK(cell_down(6, 3), "j let go at field %d while held", f);
        }
        release('j');
        fields(1);
        CHECK(!cell_down(6, 3), "j should be up after its release");
        CHECK(k.n == 0, "nothing should be held");
    }

    /* ---- the host's Shift, and characters it shifted (§9.2) ----------- */
    {
        /* Shift alone is the Ace's SHIFT, which games read on its own. */
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        fields(1);
        CHECK(shift_down() && !sym_down(), "Shift alone is SHIFT");
        release(PICOCALC_KEY_SHIFT_L);
        fields(ACE_KEY_MIN_FIELDS);
        CHECK(matrix_empty(), "and lets it go");

        /* A tap of Shift or Ctrl inside one poll is held as long as a
         * key, not applied and undone before the matrix is driven. */
        static const uint8_t taps[] = { PICOCALC_KEY_SHIFT_L, PICOCALC_KEY_SHIFT_R,
                                        PICOCALC_KEY_CTRL };
        for (unsigned i = 0; i < 3; i++) {
            fresh();
            press(taps[i]);
            release(taps[i]);
            unsigned down = 0;
            for (int f = 0; f < 20; f++) {
                fields(1);
                if (taps[i] == PICOCALC_KEY_CTRL ? sym_down() : shift_down()) down++;
            }
            CHECK(down == ACE_KEY_MIN_FIELDS, "a tap of 0x%02X held %u fields, want %u",
                  taps[i], down, (unsigned)ACE_KEY_MIN_FIELDS);
            CHECK(keymatrix_idle(&k) && matrix_empty() && !k.shift && !k.ctrl,
                  "0x%02X left down after its tap", taps[i]);
        }

        /* A modifier's held events while down do not restart its count. */
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        fields(ACE_KEY_MIN_FIELDS);
        keymatrix_event(&k, KEY_EV_HELD, PICOCALC_KEY_SHIFT_L);
        release(PICOCALC_KEY_SHIFT_L);
        fields(1);
        CHECK(!shift_down(), "Shift held its minimum already, and goes up at once");

        /* 'A' is SHIFT+A. */
        fresh();
        press(PICOCALC_KEY_SHIFT_R);
        press('A');
        fields(1);
        CHECK(cell_down(1, 0) && shift_down() && !sym_down(), "A is SHIFT+A");

        /* ':' is the PicoCalc's Shift+; and the Ace's SYMBOL SHIFT+Z:
         * SHIFT stays off the matrix while it is down. */
        fresh();
        press(PICOCALC_KEY_SHIFT_R);
        press(':');
        fields(1);
        CHECK(cell_down(0, 2) && sym_down() && !shift_down(), ": is SYMBOL SHIFT+Z alone");
        release(':');
        fields(10);
        CHECK(k.n == 0 && shift_down() && !sym_down(), "Shift still held once : is up");

        /* Releasing Shift first retranslates the release: press '"',
         * release '\'' (hardware-notes.md §6.2). It still lets go. */
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        press('"');
        release(PICOCALC_KEY_SHIFT_L);
        fields(1);
        CHECK(cell_down(5, 0) && sym_down() && !shift_down(), "\" is SYMBOL SHIFT+P");
        release('\'');
        fields(10);
        CHECK(k.n == 0 && matrix_empty(), "a release under the other translation lets go");

        /* Both Shifts: letting one go keeps SHIFT. */
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        press(PICOCALC_KEY_SHIFT_R);
        release(PICOCALC_KEY_SHIFT_L);
        fields(1);
        CHECK(shift_down(), "the other Shift is still down");

        /* A letter held first, then Shift: SHIFT joins it. */
        fresh();
        press('z');
        fields(1);
        CHECK(cell_down(0, 2) && !shift_down(), "z is Z unshifted");
        press(PICOCALC_KEY_SHIFT_R);
        press('Z');   /* the MCU's repeat */
        fields(1);
        CHECK(cell_down(0, 2) && shift_down(), "Z held with Shift");
    }

    /* ---- Ctrl is SYMBOL SHIFT (§9.2) ---------------------------------- */
    {
        fresh();
        press(PICOCALC_KEY_CTRL);
        fields(1);
        CHECK(sym_down() && !shift_down(), "Ctrl alone is SYMBOL SHIFT");
        press('i');
        fields(1);
        CHECK(sym_down() && cell_down(5, 2), "Ctrl+i reaches SYMBOL SHIFT+I");
        release(PICOCALC_KEY_CTRL);
        fields(ACE_KEY_MIN_FIELDS);
        CHECK(!sym_down() && cell_down(5, 2), "Ctrl let go, i still held");
    }

    /* ---- the editing keys, the Alt layer and the requests ------------- */
    {
        /* Arrows: SHIFT+5 left, +6 up, +7 down, +8 right (§9.3). */
        static const struct { uint8_t code, col; } arrows[] = {
            { PICOCALC_KEY_LEFT, 4 }, { PICOCALC_KEY_UP, 4 },
            { PICOCALC_KEY_DOWN, 3 }, { PICOCALC_KEY_RIGHT, 2 },
        };
        for (unsigned i = 0; i < 4; i++) {
            fresh();
            press(arrows[i].code);
            fields(1);
            unsigned row = i == 0 ? 3u : 4u;
            CHECK(cell_down((uint8_t)row, arrows[i].col) && shift_down(),
                  "arrow 0x%02X is SHIFT+(%u,%u)", arrows[i].code, row, arrows[i].col);
        }

        /* Alt+L is CAPS LOCK, SHIFT+2, not a shifted L. */
        fresh();
        press(PICOCALC_KEY_ALT);
        press('L');
        fields(1);
        CHECK(cell_down(3, 1) && shift_down() && !cell_down(6, 1), "Alt+L is SHIFT+2");

        /* The binding is fixed at press: let Alt go first, and the key's
         * release, arriving as 'l', undoes SHIFT+2 and never presses L. */
        release(PICOCALC_KEY_ALT);
        fields(1);
        CHECK(cell_down(3, 1) && !cell_down(6, 1), "L keeps its Alt binding once Alt is up");
        release('l');
        fields(10);
        CHECK(k.n == 0 && matrix_empty(), "and its release lets go of SHIFT+2");

        /* Insert is Alt+I as well as Shift+Enter (hardware-notes.md
         * §6.3). Let Alt go first and I's release arrives as 'i': it must
         * still close the press, or the next Enter is taken for a repeat. */
        fresh();
        press(PICOCALC_KEY_ALT);
        press(PICOCALC_KEY_INSERT);
        release(PICOCALC_KEY_ALT);
        release('i');
        fields(10);
        CHECK(k.n_open == 0 && keymatrix_idle(&k), "Alt+I left %u press(es) open", k.n_open);
        press(PICOCALC_KEY_ENTER);
        fields(1);
        CHECK(cell_down(6, 0), "Enter after Alt+I is not taken for a repeat");

        /* The sequence the device sent, captured 2026-10-03
         * (out/m6-soak.log): Alt let go while I was down, then the MCU's
         * auto-repeat retranslated as presses of 'i', then 'i' released. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_HELD, PICOCALC_KEY_ALT);
        press(PICOCALC_KEY_INSERT);
        keymatrix_event(&k, KEY_EV_HELD, PICOCALC_KEY_ALT);
        release(PICOCALC_KEY_ALT);
        for (int i = 0; i < 4; i++) press('i');
        release('i');
        fields(10);
        CHECK(k.n_open == 0 && keymatrix_idle(&k) && matrix_empty(),
              "the captured Alt+I left %u press(es) open", k.n_open);
        CHECK(k.q_len == 0, "the repeats were not absorbed");

        /* With Alt still down, I's release is Insert again: same key. */
        fresh();
        press(PICOCALC_KEY_ALT);
        press(PICOCALC_KEY_INSERT);
        release(PICOCALC_KEY_INSERT);
        release(PICOCALC_KEY_ALT);
        fields(10);
        CHECK(k.n_open == 0, "Alt+I released under Alt left a press open");

        /* Shift+Enter is Insert too, and its release under the other
         * translation, Enter, closes it. */
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        press(PICOCALC_KEY_INSERT);
        release(PICOCALC_KEY_SHIFT_L);
        release(PICOCALC_KEY_ENTER);
        fields(10);
        CHECK(k.n_open == 0 && keymatrix_idle(&k), "Shift+Enter left %u press(es) open",
              k.n_open);

        /* An Alt chord with no binding reaches nothing. */
        fresh();
        press(PICOCALC_KEY_ALT);
        press('Q');
        fields(1);
        CHECK(k.n == 0 && matrix_empty(), "Alt+Q should do nothing");

        /* Alt+M, Alt+H, Alt+P and Alt+K ask, as pico-atom's do, and touch
         * nothing in the machine. */
        static const struct { uint8_t code; bool *flag; const char *what; } asks[] = {
            { 'M', &k.menu_request,  "menu"  },
            { 'H', &k.menu_request,  "keys page" },
            { 'P', &k.pause_request, "pause" },
            { 'K', &k.reset_request, "reset" },
        };
        for (unsigned i = 0; i < 4; i++) {
            fresh();
            press(PICOCALC_KEY_ALT);
            press(asks[i].code);
            fields(1);
            CHECK(*asks[i].flag, "Alt+%c requests the %s", asks[i].code, asks[i].what);
            CHECK(k.menu_request + k.pause_request + k.reset_request == 1,
                  "Alt+%c requests only the %s", asks[i].code, asks[i].what);
            CHECK(matrix_empty(), "Alt+%c reached the matrix", asks[i].code);
        }
        fresh();
        press(PICOCALC_KEY_ALT);
        press('M');
        fields(1);
        CHECK(k.menu_page == KM_PAGE_MAIN, "Alt+M opens the main page");
        fresh();
        press(PICOCALC_KEY_ALT);
        press('H');
        fields(1);
        CHECK(k.menu_page == KM_PAGE_HELP, "Alt+H opens the keys page");
        fresh();
        press(PICOCALC_KEY_ALT);
        press('R');
        fields(1);
        CHECK(!k.reset_request && !k.menu_request && matrix_empty(), "Alt+R is nothing now");

        /* F1-F5 open pico-atom's pages (§12), with or without Alt held;
         * F2, its Discs, is nothing, as is F6, Shift+F1; F10 is About. */
        static const uint8_t pages[5] = {
            KM_PAGE_TAPE, 0xFF, KM_PAGE_SNAPSHOT, KM_PAGE_SETUP, KM_PAGE_MACHINE,
        };
        for (unsigned alt = 0; alt < 2; alt++) {
            for (unsigned f = 0; f < 5; f++) {
                fresh();
                if (alt) press(PICOCALC_KEY_ALT);
                press((uint8_t)(PICOCALC_KEY_F1 + f));
                fields(1);
                if (pages[f] == 0xFF)
                    CHECK(!k.menu_request, "%sF%u requests a page", alt ? "Alt+" : "", f + 1);
                else
                    CHECK(k.menu_request && k.menu_page == pages[f], "%sF%u: page %u, want %u",
                          alt ? "Alt+" : "", f + 1, k.menu_page, pages[f]);
                CHECK(matrix_empty(), "F%u reached the matrix", f + 1);
            }
        }
        fresh();
        press(0x86u);
        fields(1);
        CHECK(!k.menu_request, "F6 requests nothing");
        fresh();
        press(PICOCALC_KEY_SHIFT_L);
        press(PICOCALC_KEY_F10);
        fields(1);
        CHECK(k.menu_request && k.menu_page == KM_PAGE_ABOUT, "F10 opens About");
    }

    /* ---- the queue is bounded, and never loses a release -------------- */
    {
        /* Auto-repeat of a key already down is absorbed at once. */
        fresh();
        for (unsigned i = 0; i < ACE_KEY_EVENT_QUEUE + 5u; i++) press('x');
        CHECK(k.q_len == 1 && k.dropped == 0, "repeats: q_len %u, dropped %u", k.q_len,
              (unsigned)k.dropped);
        release('x');

        /* A burst far faster than the replay: presses are refused once
         * the queue is short of room, but every key that went down comes
         * back up, so nothing is left held. */
        fresh();
        static const char burst[] = "the quick brown fox jumps over the lazy dog 0123456789";
        for (int rep = 0; rep < 3; rep++) {
            for (const char *c = burst; *c; c++) {
                press((uint8_t)*c);
                release((uint8_t)*c);
            }
        }
        CHECK(k.dropped > 0, "the burst should have overflowed");
        CHECK(k.q_len + k.n_open <= ACE_KEY_EVENT_QUEUE, "a release has no room");
        for (int f = 0; f < 2000 && !keymatrix_idle(&k); f++) fields(1);
        CHECK(keymatrix_idle(&k) && k.n_open == 0, "stuck keys: %u held, %u open", k.n,
              k.n_open);
        CHECK(matrix_empty(), "the matrix is left with keys down");

        /* The same, with Shift and Alt in the burst: a modifier's release
         * is kept like any other. */
        fresh();
        for (int i = 0; i < 100; i++) {
            press(PICOCALC_KEY_ALT);
            press(PICOCALC_KEY_SHIFT_L);
            press('L');
            release('L');
            release(PICOCALC_KEY_SHIFT_L);
            release(PICOCALC_KEY_ALT);
        }
        for (int f = 0; f < 2000 && !keymatrix_idle(&k); f++) fields(1);
        CHECK(!k.alt && !k.shift && k.n == 0 && k.n_open == 0 && matrix_empty(),
              "a modifier left down after a burst");
    }

    /* ---- game layouts: the built-ins (§9.4) --------------------------- */
    CHECK(keylayout_builtin_len >= 2 && strcmp(keylayout_builtin[0].name, "CURSOR") == 0 &&
              strcmp(keylayout_builtin[1].name, "QAOP") == 0,
          "Cursor and QAOP are the built-in layouts");
    for (size_t li = 0; li < keylayout_builtin_len; li++) {
        const keylayout_t *l = &keylayout_builtin[li];
        CHECK(l->n <= ACE_KEYMAP_BINDINGS && l->n_tapes == 0,
              "%s: over config.h's capacities, or names a game", l->name);
        for (unsigned i = 0; i < l->n; i++) {
            const keymap_t *e = &l->bind[i];
            CHECK(keymap_picocalc_canonical(e->code) == e->code,
                  "%s: 0x%02X is not a canonical code, so it would never match", l->name,
                  e->code);
            CHECK(e->flags == 0, "%s: 0x%02X carries flags", l->name, e->code);
            CHECK(e->row < ACE_KEY_ROWS && e->col < ACE_KEY_COLS, "%s: off the matrix",
                  l->name);
            for (unsigned j = i + 1; j < l->n; j++) {
                CHECK(l->bind[j].code != e->code, "%s: 0x%02X bound twice", l->name, e->code);
            }
        }
    }

    /* ---- game layouts: the overlay in the held set -------------------- */
    {
        const keylayout_t *cursor = &keylayout_builtin[0];

        /* Left is 5, SHIFT up: the game sees the cell. The standard map's
         * Left is SHIFT+5, the control. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press(PICOCALC_KEY_LEFT);
        fields(1);
        CHECK(cell_down(3, 4) && !shift_down(), "Left under Cursor is 5, unshifted");
        fresh();
        press(PICOCALC_KEY_LEFT);
        fields(1);
        CHECK(cell_down(3, 4) && shift_down(), "Left under the standard map is SHIFT+5");

        /* Moving and firing: both cells, and each lets go on its own
         * release. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press(PICOCALC_KEY_RIGHT);
        press(']');
        fields(1);
        CHECK(cell_down(4, 2) && cell_down(4, 0) && !shift_down() && !sym_down(),
              "Right and ] held: 8 and 0 together");
        fields(10);
        release(PICOCALC_KEY_RIGHT);
        fields(1);
        CHECK(!cell_down(4, 2) && cell_down(4, 0), "Right let go, ] still held");
        release(']');
        fields(1);
        CHECK(matrix_empty() && k.n == 0, "both let go");

        /* Shifted, ']' arrives as '}', and is the same key; a layout's
         * cell takes the host's Shift as it finds it. The standard map's
         * '}' is SYMBOL SHIFT+G. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press(PICOCALC_KEY_SHIFT_L);
        press('}');
        fields(1);
        CHECK(cell_down(4, 0) && shift_down() && !cell_down(1, 4) && !sym_down(),
              "} is fire too, under SHIFT");

        /* An unmentioned key keeps its standard binding. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press('r');
        fields(1);
        CHECK(cell_down(2, 3) && !shift_down(), "r keeps R");

        /* The Alt layer and the F-keys are never overlaid. */
        static keylayout_t greedy;
        unsigned bad = 0;
        const char *gr = "m = Q\nf = A\n";
        CHECK(keylayout_parse(&greedy, "GREEDY", gr, strlen(gr), &bad) == KL_OK,
              "greedy parses");
        fresh();
        keymatrix_set_layout(&k, &greedy);
        press(PICOCALC_KEY_ALT);
        press('M');
        fields(1);
        CHECK(k.menu_request && !cell_down(2, 0), "Alt+M is the menu under any layout");
        fresh();
        keymatrix_set_layout(&k, &greedy);
        press(PICOCALC_KEY_F1 + 2);
        fields(1);
        CHECK(k.menu_request && k.menu_page == KM_PAGE_SNAPSHOT,
              "F3 is the menu under any layout");
        fresh();
        keymatrix_set_layout(&k, &greedy);
        press('M');   /* Shift+m */
        fields(1);
        CHECK(cell_down(2, 0) && !k.menu_request, "a layout binds the key, shifted or not");

        /* A key keeps the binding it went down with: its release undoes
         * that even when the layout changed in between. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press(PICOCALC_KEY_RIGHT);
        fields(1);
        keymatrix_set_layout(&k, NULL);
        fields(1);
        CHECK(cell_down(4, 2) && !shift_down(), "Right held keeps 8 across a change");
        release(PICOCALC_KEY_RIGHT);
        fields(10);
        CHECK(matrix_empty() && k.n == 0, "and lets go of it");
        press(PICOCALC_KEY_RIGHT);
        fields(3);
        CHECK(cell_down(4, 2) && shift_down(), "the next press takes the standard map");

        /* The pacing and bounds of the held set hold with a layout. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        press(']');
        release(']');
        unsigned down = 0;
        for (int f = 0; f < 20; f++) {
            fields(1);
            if (cell_down(4, 0)) down++;
        }
        CHECK(down == ACE_KEY_MIN_FIELDS, "a tap of fire is %u fields of 0, want %u", down,
              (unsigned)ACE_KEY_MIN_FIELDS);
        fresh();
        keymatrix_set_layout(&k, cursor);
        for (int rep = 0; rep < 60; rep++) {
            press(PICOCALC_KEY_LEFT);
            press(']');
            release(PICOCALC_KEY_LEFT);
            press('z');
            release(']');
            release('z');
        }
        for (int f = 0; f < 4000 && !keymatrix_idle(&k); f++) fields(1);
        CHECK(keymatrix_idle(&k) && k.n_open == 0 && matrix_empty(),
              "stuck keys under a layout: %u held, %u open", k.n, k.n_open);
    }

    /* ---- game layouts: the .map parser -------------------------------- */
    {
        static keylayout_t l;
        unsigned line = 99;

        /* The built-in Cursor, written as a file (§9.4). */
        const char *ex = "# Cursor: the Ace's arrows on the PicoCalc's, fire on ]\n"
                         "name  = CURSOR\n"
                         "left  = 5\n"
                         "up    = 6\n"
                         "down  = 7\n"
                         "right = 8\n"
                         "]     = 0\n";
        CHECK(keylayout_parse(&l, "mine", ex, strlen(ex), &line) == KL_OK && line == 0,
              "the example parses");
        const keylayout_t *b = &keylayout_builtin[0];
        CHECK(strcmp(l.name, b->name) == 0 && l.n == b->n && l.n_tapes == b->n_tapes,
              "the example is the built-in layout");
        CHECK(memcmp(l.bind, b->bind, sizeof(keymap_t) * b->n) == 0, "same bindings");
        CHECK(!keylayout_for_file(&l, "/ace/tapes/CURSOR.tap") && !keylayout_for_file(&l, ""),
              "no tapes line, no file selects it");

        /* A card file may name its own files, as .tap or .ace. */
        const char *tp = "left = O\ntapes = Invaders, ROCKET\n";
        CHECK(keylayout_parse(&l, "mine", tp, strlen(tp), &line) == KL_OK && l.n_tapes == 2,
              "a tapes line parses");
        CHECK(keylayout_for_file(&l, "/ace/tapes/INVADERS.tap") &&
                  keylayout_for_file(&l, "/ace/snaps/rocket.ace") &&
                  keylayout_for_file(&l, "invaders"),
              "the files it names select it, ignoring case and the extension");
        CHECK(!keylayout_for_file(&l, "/ace/tapes/INVADER.tap") &&
                  !keylayout_for_file(&l, "/ace/tapes/ROCKETS.tap") &&
                  !keylayout_for_file(&l, "/ace/tapes/.tap") && !keylayout_for_file(&l, ""),
              "nothing else does");

        /* '#' is a comment, unless it is the key being bound. */
        const char *hash = "# a comment = not a binding\n#=SPACE\n  # indented comment\n";
        CHECK(keylayout_parse(&l, "hash", hash, strlen(hash), &line) == KL_OK && l.n == 1,
              "# binds once, line %u, %u binding(s)", line, l.n);
        CHECK(l.bind[0].code == keymap_picocalc_canonical('#') && l.bind[0].row == 7 &&
                  l.bind[0].col == 0,
              "# = SPACE binds the # key to SPACE");

        /* CRLF, blank lines, no final newline, the file's own name, and
         * SHIFT and SYMBOL SHIFT as targets. */
        const char *crlf = "\r\n  A = SPACE\r\n\r\n\tSPACE=z\r\n= = shift\r\n; = Symbol";
        CHECK(keylayout_parse(&l, "fire", crlf, strlen(crlf), &line) == KL_OK,
              "CRLF parses, line %u", line);
        CHECK(strcmp(l.name, "FIRE") == 0 && l.n == 4, "name from the file name, 4 bindings");
        CHECK(l.bind[0].code == 'a' && l.bind[0].row == 7 && l.bind[0].col == 0 &&
                  l.bind[0].flags == 0, "A = SPACE");
        CHECK(l.bind[1].code == ' ' && l.bind[1].row == 0 && l.bind[1].col == 2, "space = Z");
        CHECK(l.bind[2].code == '=' && l.bind[2].row == AK_ROW_MODS &&
                  l.bind[2].col == AK_COL_SHIFT && l.bind[2].flags == 0,
              "'=' binds SHIFT, as a cell");
        CHECK(l.bind[3].code == ';' && l.bind[3].row == AK_ROW_MODS &&
                  l.bind[3].col == AK_COL_SYM, "; = SYMBOL SHIFT");

        /* Every failure names its line, and nothing is guessed. */
        static const struct { const char *text; keylayout_status_t st; unsigned line; } bad[] = {
            { "left = 5\nright\n",               KL_SYNTAX,     2 },
            { "left =\n",                        KL_SYNTAX,     1 },
            { "left up = 5\n",                   KL_SYNTAX,     1 },
            { "\n\nf1 = 5\n",                    KL_BAD_KEY,    3 },
            { "left = BREAK\n",                  KL_BAD_TARGET, 1 },
            { "left = 5 6\n",                    KL_BAD_TARGET, 1 },
            { "left = 5\nLEFT = 6\n",            KL_DUPLICATE,  2 },
            { "a = 5\nA = 6\n",                  KL_DUPLICATE,  2 },
            { "name = A NAME FAR TOO LONG\n",    KL_TOO_LONG,   1 },
            { "tapes = A B C D E\n",             KL_TOO_MANY,   1 },
            { "tapes = SEVENTEEN_LETTERS\n",     KL_TOO_LONG,   1 },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            keylayout_status_t st = keylayout_parse(&l, "x", bad[i].text, strlen(bad[i].text),
                                                    &line);
            CHECK(st == bad[i].st && line == bad[i].line,
                  "\"%s\": got %s at line %u, want %s at line %u", bad[i].text,
                  keylayout_status_str(st), line, keylayout_status_str(bad[i].st),
                  bad[i].line);
        }

        /* Every built-in binding, written back as the menu shows it,
         * parses to itself. */
        for (size_t li = 0; li < keylayout_builtin_len; li++) {
            const keylayout_t *bl = &keylayout_builtin[li];
            static char text[512];
            size_t at = 0;
            for (unsigned i = 0; i < bl->n; i++) {
                char one[24];
                keymap_binding_str(&bl->bind[i], one, sizeof one);
                at += (size_t)snprintf(text + at, sizeof text - at, "%s\n", one);
            }
            CHECK(keylayout_parse(&l, bl->name, text, at, &line) == KL_OK && l.n == bl->n &&
                      memcmp(l.bind, bl->bind, sizeof(keymap_t) * bl->n) == 0,
                  "%s written back does not parse to itself:\n%s", bl->name, text);
        }
        char one[24];
        keymap_binding_str(&keylayout_builtin[0].bind[0], one, sizeof one);
        CHECK(strcmp(one, "left=5") == 0, "Cursor's first binding reads \"%s\"", one);

        /* One more binding than config.h allows. */
        static char many[1024];
        size_t at = 0;
        for (unsigned i = 0; i <= ACE_KEYMAP_BINDINGS; i++) {
            at += (size_t)snprintf(many + at, sizeof many - at, "%c = SPACE\n", 'a' + i);
        }
        CHECK(keylayout_parse(&l, "x", many, at, &line) == KL_TOO_MANY &&
                  line == ACE_KEYMAP_BINDINGS + 1u, "too many bindings");
    }

    TEST_DONE();
}
