/* test_keyboard.c — the keyboard, settled by executing the ROM (design.md
 * §9.3, §15.2 M5).
 *
 * 1. The sweep: every cell of the matrix pressed at the prompt alone,
 *    with SHIFT, with SYMBOL SHIFT and with both, and what the ROM made
 *    of it.
 * 2. Every entry of the PicoCalc table typed through keymatrix into the
 *    ROM, the editing keys by what they do to the input line.
 * 3. The hold and gap the ROM's scan needs, with controls that must fail.
 */

#include <string.h>

#include "guest.h"
#include "test_util.h"

static guest_t g;
static ace_t   booted;

/* The key the ROM's scan decoded, from the first field that sees it
 * until it is let go (found by execution, 2026-10-03). */
#define ROM_LASTK 0x3C26u

/* The input line on a fresh prompt starts at column 1 of the bottom row. */
#define INPUT_ADDR ((ACE_SCREEN_ROWS - 1u) * ACE_SCREEN_COLS + 1u)

static void fresh(void) {
    ace_copy(&g.m, &booted);
    keymatrix_init(&g.k);
}

/* The cursor in each mode: plain, CAPS LOCK (an inverse C) and GRAPHICS
 * (an inverse G), ROM $0282. */
static bool is_cursor(uint8_t v) {
    return v == GUEST_CURSOR || v == 0xC3u || v == 0xC7u;
}

/* The input line up to the cursor, as typed: one line, raw bytes. */
static const char *input(void) {
    static char s[ACE_SCREEN_COLS];
    const uint8_t *p = ace_screen(&g.m) + INPUT_ADDR;
    unsigned n = 0;
    while (n < ACE_SCREEN_COLS - 1u && !is_cursor(p[n])) {
        s[n] = (char)p[n];
        n++;
    }
    s[n] = 0;
    return s;
}

/* Where a byte first appears in the screen from `addr` on, or -1. */
static int find_from(unsigned addr, uint8_t v) {
    const uint8_t *scr = ace_screen(&g.m);
    for (unsigned a = addr; a < ACE_SCREEN_BYTES; a++)
        if (scr[a] == v) return (int)(a - addr);
    return -1;
}

/* ---- 1. the sweep ------------------------------------------------------ */

/* What each cell types: alone, with SHIFT, with SYMBOL SHIFT. Read off
 * the ROM by this sweep and kept as its regression (§2.4, §9.3). The
 * control codes are the editing keys: $01 left, $02 CAPS LOCK, $03 right,
 * $04 GRAPHICS, $05 DELETE, $07 up, $08 INVERSE VIDEO, $09 down,
 * $0A DELETE LINE, $0D ENTER. */
static const uint8_t sweep[ACE_KEY_ROWS][ACE_KEY_COLS][3] = {
    { { 0 }, { 0 },          { 'z', 'Z', ':' }, { 'x', 'X', 0x60 }, { 'c', 'C', '?' } },
    { { 'a', 'A', '~' },  { 's', 'S', '|' },  { 'd', 'D', '\\' }, { 'f', 'F', '{' },
      { 'g', 'G', '}' } },
    { { 'q', 'Q', 'Q' },  { 'w', 'W', 'W' },  { 'e', 'E', 'E' },  { 'r', 'R', '<' },
      { 't', 'T', '>' } },
    { { '1', 0x0A, '!' }, { '2', 0x02, '@' }, { '3', '3', '#' },  { '4', 0x08, '$' },
      { '5', 0x01, '%' } },
    { { '0', 0x05, '_' }, { '9', 0x04, ')' }, { '8', 0x03, '(' }, { '7', 0x09, '\'' },
      { '6', 0x07, '&' } },
    { { 'p', 'P', '"' },  { 'o', 'O', ';' },  { 'i', 'I', 0x7F }, { 'u', 'U', ']' },
      { 'y', 'Y', '[' } },
    { { 0x0D, 0x0D, 0x0D }, { 'l', 'L', '=' }, { 'k', 'K', '+' }, { 'j', 'J', '-' },
      { 'h', 'H', '^' } },
    { { ' ', ' ', ' ' },  { 'm', 'M', '.' },  { 'n', 'N', ',' },  { 'b', 'B', '*' },
      { 'v', 'V', '/' } },
};

/* CHECK returns 1 once there are too many failures. */
static int run_sweep(void) {
    static const char *const mode[4] = { "alone", "with SHIFT", "with SYMBOL SHIFT",
                                         "with both" };
    for (unsigned r = 0; r < ACE_KEY_ROWS; r++) {
        for (unsigned c = 0; c < ACE_KEY_COLS; c++) {
            if (r == AK_ROW_MODS && c <= AK_COL_SYM) continue;
            /* SHIFT with SYMBOL SHIFT types what SYMBOL SHIFT alone does,
             * so the held set's unshift (§9.2) keeps the matrix faithful
             * rather than the ROM's text right. */
            for (unsigned md = 0; md < 4; md++) {
                fresh();
                /* Straight into the matrix, not through keymatrix: this
                 * is the ROM's map, before ours. */
                if (md & 1u) ace_key_set(&g.m, AK_ROW_MODS, AK_COL_SHIFT, true);
                if (md & 2u) ace_key_set(&g.m, AK_ROW_MODS, AK_COL_SYM, true);
                ace_key_set(&g.m, (int)r, (int)c, true);
                for (unsigned f = 0; f < ACE_KEY_MIN_FIELDS; f++) ace_run_field(&g.m);
                uint8_t got = ace_peek(&g.m, ROM_LASTK);   /* cleared once let go */
                memset(g.m.keys, 0, sizeof g.m.keys);
                for (unsigned f = 0; f < 4; f++) ace_run_field(&g.m);

                uint8_t want = sweep[r][c][md == 3 ? 2 : md];
                CHECK(got == want, "(%u,%u) %s decodes $%02X, want $%02X", r, c, mode[md],
                      got, want);
                if (want >= 0x20u) {
                    uint8_t shown = ace_screen(&g.m)[INPUT_ADDR];
                    CHECK(shown == want, "(%u,%u) %s types $%02X, want $%02X", r, c,
                          mode[md], shown, want);
                }
            }
        }
    }
    return 0;
}

/* ---- 2. the PicoCalc table through the ROM ----------------------------- */

static int every_printable_entry(void) {
    unsigned n = 0;
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->flags & KM_ALT || e->code < 0x20u || e->code > 0x7Eu) continue;
        fresh();
        guest_press(&g, e->code, false);
        const char *in = input();
        CHECK(in[0] == (char)e->code && in[1] == 0, "'%c' types \"%s\"", e->code, in);
        n++;
    }
    CHECK(n == 0x7Fu - 0x20u, "%u printable entries typed, want %u", n, 0x7Fu - 0x20u);

    /* '©' has no PicoCalc key: Ctrl is SYMBOL SHIFT, and Ctrl+i is it. */
    fresh();
    keymatrix_event(&g.k, KEY_EV_PRESSED, PICOCALC_KEY_CTRL);
    guest_press(&g, 'i', false);
    keymatrix_event(&g.k, KEY_EV_RELEASED, PICOCALC_KEY_CTRL);
    guest_settle(&g, 10);
    CHECK(strcmp(input(), "\x7F") == 0, "Ctrl+i types \"%s\", want the ©", input());
    return 0;
}

static int editing_keys(void) {
    /* Backspace and Del are DELETE. */
    static const uint8_t deletes[] = { PICOCALC_KEY_BACKSPACE, PICOCALC_KEY_DEL };
    for (unsigned i = 0; i < 2; i++) {
        fresh();
        guest_type(&g, "ab");
        guest_press(&g, deletes[i], false);
        CHECK(strcmp(input(), "a") == 0, "ab, 0x%02X: \"%s\"", deletes[i], input());
    }

    /* Left and right. */
    fresh();
    guest_type(&g, "ab");
    guest_press(&g, PICOCALC_KEY_LEFT, false);
    guest_type(&g, "c");
    CHECK(strncmp(input(), "ac", 2) == 0 && ace_screen(&g.m)[INPUT_ADDR + 3] == 'b',
          "ab, left, c: \"%s\"", input());
    fresh();
    guest_type(&g, "ab");
    guest_press(&g, PICOCALC_KEY_LEFT, false);
    guest_press(&g, PICOCALC_KEY_RIGHT, false);
    guest_type(&g, "c");
    CHECK(strcmp(input(), "abc") == 0, "ab, left, right, c: \"%s\"", input());

    /* Up and down move 32 places in a line that spans two rows. The line
     * starts at row 22, column 1, once 40 characters have scrolled it. */
    const unsigned two_rows = (ACE_SCREEN_ROWS - 2u) * ACE_SCREEN_COLS + 1u;
    fresh();
    for (int i = 0; i < 40; i++) guest_type(&g, "a");
    guest_press(&g, PICOCALC_KEY_UP, false);
    guest_type(&g, "c");
    CHECK(find_from(two_rows, 'c') == 40 - 32, "40 a, up, c: c at %d, want 8",
          find_from(two_rows, 'c'));
    fresh();
    for (int i = 0; i < 40; i++) guest_type(&g, "a");
    for (int i = 0; i < 35; i++) guest_press(&g, PICOCALC_KEY_LEFT, false);
    guest_press(&g, PICOCALC_KEY_DOWN, false);
    guest_type(&g, "c");
    CHECK(find_from(two_rows, 'c') == 5 + 32, "40 a, 35 left, down, c: c at %d, want 37",
          find_from(two_rows, 'c'));

    /* The Alt layer. */
    fresh();
    guest_press(&g, 'L', true);
    guest_type(&g, "a");
    CHECK(strcmp(input(), "A") == 0, "Alt+L, a: \"%s\"", input());
    fresh();
    guest_press(&g, 'G', true);
    guest_type(&g, "a");
    CHECK(strcmp(input(), "\x01") == 0, "Alt+G, a: graphic $%02X", (uint8_t)input()[0]);
    fresh();
    guest_press(&g, 'V', true);
    guest_type(&g, "a");
    guest_press(&g, 'V', true);
    guest_type(&g, "a");
    CHECK(strcmp(input(), "\xE1" "a") == 0, "Alt+V, a, Alt+V, a: $%02X $%02X",
          (uint8_t)input()[0], (uint8_t)input()[1]);
    fresh();
    guest_type(&g, "ab");
    guest_press(&g, 'X', true);
    CHECK(strcmp(input(), "") == 0, "ab, Alt+X: \"%s\"", input());

    /* The requests type nothing. */
    static const uint8_t asks[] = { 'M', 'P', 'R' };
    for (unsigned i = 0; i < 3; i++) {
        fresh();
        guest_press(&g, asks[i], true);
        guest_type(&g, "a");
        CHECK(strcmp(input(), "a") == 0, "Alt+%c typed into the guest: \"%s\"", asks[i],
              input());
    }
    for (unsigned f = 0; f < 5; f++) {
        fresh();
        guest_press(&g, (uint8_t)(PICOCALC_KEY_F1 + f), false);
        CHECK(strcmp(input(), "") == 0, "F%u typed into the guest", f + 1);
    }

    /* Enter runs the line; Esc and Shift+Esc are BREAK, which stops a
     * running word with ERROR 3. */
    static const uint8_t breaks[] = { PICOCALC_KEY_ESC, PICOCALC_KEY_BREAK };
    for (unsigned i = 0; i < 2; i++) {
        fresh();
        guest_type(&g, ": x begin 0 until ; x\n");
        guest_fields(&g, 20);
        CHECK(strcmp(guest_row(&g.m, 0), ": x begin 0 until ; x") == 0,
              "the loop is not running: \"%s\"", guest_row(&g.m, 0));
        guest_press(&g, breaks[i], false);
        guest_fields(&g, 10);
        CHECK(strcmp(guest_row(&g.m, 0), ": x begin 0 until ; x ERROR 3") == 0,
              "0x%02X did not break: \"%s\"", breaks[i], guest_row(&g.m, 0));
    }
    return 0;
}

/* ---- 3. the hold and gap the ROM needs ---------------------------------- */

/* Type "aabbab112" straight into the matrix, each key held `hold` fields
 * with `gap` between, and say whether the ROM read it back. */
static bool types_at(unsigned hold, unsigned gap) {
    static const uint8_t keys[][2] = {
        { 1, 0 }, { 1, 0 }, { 7, 3 }, { 7, 3 }, { 1, 0 }, { 7, 3 }, { 3, 0 }, { 3, 0 }, { 3, 1 },
    };
    fresh();
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        ace_key_set(&g.m, keys[i][0], keys[i][1], true);
        for (unsigned f = 0; f < hold; f++) ace_run_field(&g.m);
        ace_key_set(&g.m, keys[i][0], keys[i][1], false);
        for (unsigned f = 0; f < gap; f++) ace_run_field(&g.m);
    }
    for (unsigned f = 0; f < 10; f++) ace_run_field(&g.m);
    return strcmp(input(), "aabbab112") == 0;
}

/* A replay of n keys takes a hold and a gap for each but the last, then
 * the last one's hold and the field it is let go in. */
static bool k_fields_ok(unsigned took, unsigned n) {
    return took == (n - 1u) * (ACE_KEY_MIN_FIELDS + ACE_KEY_GAP_FIELDS) + ACE_KEY_MIN_FIELDS + 1u;
}

/* How many times one key held `hold` fields is typed. */
static size_t typed_by_hold(unsigned hold) {
    fresh();
    ace_key_set(&g.m, 1, 0, true);
    for (unsigned f = 0; f < hold; f++) ace_run_field(&g.m);
    ace_key_set(&g.m, 1, 0, false);
    for (unsigned f = 0; f < 10; f++) ace_run_field(&g.m);
    return strlen(input());
}

static int hold_and_gap(void) {
    /* The least of each that types the line, the other held generous. */
    int hold = -1, gap = -1;
    for (int h = 1; h <= 8 && hold < 0; h++)
        if (types_at((unsigned)h, 4)) hold = h;
    for (int gp = 0; gp <= 4 && hold > 0 && gap < 0; gp++)
        if (types_at((unsigned)hold, (unsigned)gp)) gap = gp;
    printf("the ROM needs a key held %d fields and %d up: %d fields per key; "
           "the replay uses %u and %u\n",
           hold, gap, hold + gap, (unsigned)ACE_KEY_MIN_FIELDS, (unsigned)ACE_KEY_GAP_FIELDS);

    /* Settled: three scans down and one up (§16). The controls must fail. */
    CHECK(hold == 3 && gap == 1, "the ROM needs %d held and %d up, expected 3 and 1", hold,
          gap);
    CHECK(!types_at(2, 4), "control: 2 fields held should lose keys");
    CHECK(!types_at(3, 0), "control: no gap should lose keys");
    CHECK(types_at(ACE_KEY_MIN_FIELDS, ACE_KEY_GAP_FIELDS), "the replay's own timing fails");

    /* The ceiling: the ROM repeats a key held 33 fields, and then every 4. */
    CHECK(typed_by_hold(32) == 1 && typed_by_hold(33) == 2 && typed_by_hold(37) == 3,
          "repeat: 32 fields typed %zu, 33 typed %zu, 37 typed %zu", typed_by_hold(32),
          typed_by_hold(33), typed_by_hold(37));
    CHECK(ACE_KEY_MIN_FIELDS < 33u, "the replay's hold would repeat");

    /* Through keymatrix, a line arriving in one poll, as a fast typist's
     * would: the replay's own rate, in fields per key. */
    fresh();
    const char *line = "2 2 + .";
    for (const char *c = line; *c; c++) {
        uint8_t code = (uint8_t)*c;
        bool shift = keymap_picocalc_canonical(code) != code;
        if (shift) keymatrix_event(&g.k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&g.k, KEY_EV_PRESSED, code);
        keymatrix_event(&g.k, KEY_EV_RELEASED, code);
        if (shift) keymatrix_event(&g.k, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
    }
    keymatrix_event(&g.k, KEY_EV_PRESSED, PICOCALC_KEY_ENTER);
    keymatrix_event(&g.k, KEY_EV_RELEASED, PICOCALC_KEY_ENTER);
    uint32_t f0 = g.m.fields;
    CHECK(guest_settle(&g, 200), "the line was not replayed in 200 fields");
    unsigned keys = (unsigned)strlen(line) + 1u;
    unsigned took = (unsigned)(g.m.fields - f0) - ACE_KEY_GAP_FIELDS;
    printf("a line of %u keys in one poll replays in %u fields, %.1f a key\n", keys, took,
           (double)took / keys);
    CHECK(k_fields_ok(took, keys), "replay took %u fields for %u keys", took, keys);
    guest_fields(&g, 10);
    CHECK(strcmp(guest_row(&g.m, 0), "2 2 + . 4  OK") == 0, "top line \"%s\"",
          guest_row(&g.m, 0));
    return 0;
}

int main(void) {
    CHECK(guest_boot(&g, ACE_RAM_19K, 500), "no cursor in 500 fields");
    ace_copy(&booted, &g.m);

    if (run_sweep() || every_printable_entry() || editing_keys() || hold_and_gap()) return 1;

    TEST_DONE();
}
