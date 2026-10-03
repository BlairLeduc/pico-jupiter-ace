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

        /* Alt+M, Alt+P and Alt+R ask, and touch nothing in the machine. */
        static const struct { uint8_t code; bool *flag; const char *what; } asks[] = {
            { 'M', &k.menu_request,  "menu"  },
            { 'P', &k.pause_request, "pause" },
            { 'R', &k.reset_request, "reset" },
        };
        for (unsigned i = 0; i < 3; i++) {
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

        /* F1-F5 open their pages (§12), with or without Alt held; F6,
         * Shift+F1, is nothing; F10 is the menu. */
        static const uint8_t pages[5] = {
            KM_PAGE_TAPE, KM_PAGE_SNAPSHOT, KM_PAGE_MACHINE, KM_PAGE_LAYOUT, KM_PAGE_ABOUT,
        };
        for (unsigned alt = 0; alt < 2; alt++) {
            for (unsigned f = 0; f < 5; f++) {
                fresh();
                if (alt) press(PICOCALC_KEY_ALT);
                press((uint8_t)(PICOCALC_KEY_F1 + f));
                fields(1);
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
        CHECK(k.menu_request && k.menu_page == KM_PAGE_MAIN, "F10 opens the menu");
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

    TEST_DONE();
}
