/* test_render.c — the row generator, the emulator's font, power-on, and
 * the glyph-change dirty bands (design.md §7, §13.2 Video).
 *
 * The band diff is checked by executing it: a simulated panel is brought
 * up to date by drawing only the spans render_diff reports, and must then
 * match a full render of the same snapshot. A span that is too narrow, or
 * a band it forgot, leaves a stale pixel and fails. The same run with a
 * screen-only diff, which ignores the character set, is the control: it
 * must leave stale pixels, or the test cannot tell the two apart (§7.3).
 */

#include <string.h>

#include "font.h"
#include "guest.h"
#include "render.h"
#include "test_util.h"

static render_t g_r;
static uint16_t g_panel[ACE_PIXEL_H][ACE_PIXEL_W];
static uint16_t g_full[ACE_PIXEL_H][ACE_PIXEL_W];
static uint8_t  g_screen[ACE_SCREEN_BYTES];
static uint8_t  g_charset[ACE_CHARSET_BYTES];
static render_shadow_t g_shadow;
static guest_t g;

static void render_full(const uint8_t *screen, const uint8_t *charset,
                        uint16_t (*dst)[ACE_PIXEL_W]) {
    for (unsigned y = 0; y < ACE_PIXEL_H; y++) render_row(&g_r, screen, charset, y, dst[y]);
}

/* What the presenter does per snapshot, minus the wire: each dirty band's
 * span, 8 rows of it, generated with render_cells as the port will. */
static void draw_bands(const render_band_t *bands) {
    uint16_t row[ACE_PIXEL_W];
    for (unsigned b = 0; b < ACE_BAND_COUNT; b++) {
        unsigned c0 = bands[b].c0, c1 = bands[b].c1;
        if (c0 > c1) continue;
        for (unsigned r = 0; r < ACE_BAND_ROWS; r++) {
            unsigned y = b * ACE_BAND_ROWS + r;
            render_cells(&g_r, g_screen, g_charset, y, c0, c1, row);
            memcpy(&g_panel[y][c0 * 8u], row, (c1 - c0 + 1u) * 8u * sizeof(uint16_t));
        }
    }
}

static unsigned present(void) {
    render_band_t bands[ACE_BAND_COUNT];
    unsigned n = render_diff(&g_shadow, g_screen, g_charset, bands);
    draw_bands(bands);
    return n;
}

/* The control: the same present with the screen bytes alone. */
static void present_screen_only(void) {
    static const glyph_mask_t none;
    render_band_t bands[ACE_BAND_COUNT];
    for (unsigned b = 0; b < ACE_BAND_COUNT; b++) {
        unsigned c0 = 1, c1 = 0;
        render_band_span(g_screen, g_shadow.screen, &none, b, &c0, &c1);
        bands[b].c0 = (uint8_t)c0;
        bands[b].c1 = (uint8_t)c1;
    }
    draw_bands(bands);
    memcpy(g_shadow.screen, g_screen, sizeof g_screen);
    memcpy(g_shadow.charset, g_charset, sizeof g_charset);
}

static unsigned rng_state = 12345;
static unsigned rng(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

/* Random edits, then one present, repeated; true if the panel ever
 * differed from a full render. Every third step changes only the
 * character set, so the screen bytes are identical across the present. */
static bool run_edits(bool screen_only, unsigned steps) {
    for (unsigned i = 0; i < sizeof g_screen; i++) g_screen[i] = (uint8_t)rng();
    for (unsigned i = 0; i < sizeof g_charset; i++) g_charset[i] = (uint8_t)rng();
    g_shadow.valid = false;
    present();

    bool stale = false;
    for (unsigned step = 0; step < steps; step++) {
        unsigned n = 1u + rng() % 6u;
        for (unsigned k = 0; k < n; k++) {
            if (step % 3u == 0 || (rng() & 1u))
                g_charset[rng() % sizeof g_charset] ^= (uint8_t)(1u + rng() % 255u);
            else
                g_screen[rng() % sizeof g_screen] ^= (uint8_t)(1u + rng() % 255u);
        }
        if (screen_only) present_screen_only();
        else present();
        render_full(g_screen, g_charset, g_full);
        if (memcmp(g_panel, g_full, sizeof g_panel) != 0) stale = true;
    }
    return stale;
}

int main(void) {
    render_init(&g_r, ACE_INK_RGB565, ACE_PAPER_RGB565);
    const uint16_t I = ACE_INK_RGB565, P = ACE_PAPER_RGB565;

    /* ---- the row generator: bit 7 leftmost, bit 7 of the code inverts - */
    {
        memset(g_screen, 0x20, sizeof g_screen);
        g_screen[1] = 'A';
        g_screen[2] = 'A' | 0x80u;
        uint16_t row[ACE_PIXEL_W];
        render_row(&g_r, g_screen, ace_font, 0, row);   /* 'A' row 0: $30 */
        static const uint16_t a0[8] = { 0, 0, 1, 1, 0, 0, 0, 0 };
        bool ok = true;
        for (unsigned x = 0; x < 8u; x++) {
            ok &= row[8u + x] == (a0[x] ? I : P);
            ok &= row[16u + x] == (a0[x] ? P : I);
            ok &= row[x] == P;   /* space */
        }
        CHECK(ok, "'A' row 0 should be ..XX.... at cell 1 and its inverse at cell 2");

        uint16_t part[2 * 8];
        render_cells(&g_r, g_screen, ace_font, 0, 1, 2, part);
        CHECK(memcmp(part, &row[8], sizeof part) == 0,
              "render_cells for cells 1..2 should equal those cells of render_row");
    }

    /* ---- the emulator's font, against its source (EL §5.5) ----------- */
    {
        /* third_party/font8x8/README draws 'A' as 0C 1E 33 33 3F 33 33 00
         * with bit 0 leftmost; here bit 7 is. */
        static const uint8_t a[8] = { 0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00 };
        CHECK(memcmp(&ace_font['A' * 8], a, 8) == 0, "the font's 'A' is not the README's");

        uint8_t or_all = 0, blank = 0;
        for (unsigned c = 0x21; c < 0x7Fu; c++)
            for (unsigned y = 0; y < 8u; y++) or_all |= ace_font[c * 8u + y];
        for (unsigned y = 0; y < 8u; y++) blank |= ace_font[' ' * 8u + y];
        CHECK(or_all & 0x80u, "no printable glyph reaches the left column: shifted right?");
        CHECK(blank == 0, "space is not blank");
    }

    /* ---- power-on: zeroed character RAM, drawn as it is (§7.5) ------- */
    {
        ace_config_t cfg;
        guest_config(&cfg, ACE_RAM_19K);
        CHECK(ace_init(&g.m, &cfg), "ace_init");
        render_full(ace_screen(&g.m), ace_charset(&g.m), g_full);
        bool all_paper = true;
        for (unsigned y = 0; y < ACE_PIXEL_H; y++)
            for (unsigned x = 0; x < ACE_PIXEL_W; x++) all_paper &= g_full[y][x] == P;
        CHECK(all_paper, "at power-on, zeroed screen and character RAM draw all paper");

        /* Nothing substitutes for the empty glyph: an inverse cell of it
         * is solid ink, not a blank. */
        memcpy(g_screen, ace_screen(&g.m), sizeof g_screen);
        g_screen[0] = 0x80u;
        uint16_t row[ACE_PIXEL_W];
        render_row(&g_r, g_screen, ace_charset(&g.m), 7, row);
        bool solid = true;
        for (unsigned x = 0; x < 8u; x++) solid &= row[x] == I;
        CHECK(solid && row[8] == P, "code $80 over zeroed character RAM is a solid ink cell");

        /* When the ROM first writes the character set, for the record. */
        int f = 0;
        bool written = false;
        for (; f < 50 && !written; f++) {
            ace_run_field(&g.m);
            for (unsigned i = 0; i < ACE_CHARSET_BYTES && !written; i++)
                written = ace_charset(&g.m)[i] != 0;
        }
        CHECK(written, "the ROM wrote no character set in 50 fields");
        printf("character set first non-zero after %d field(s)\n", f);
    }

    /* ---- a redefined glyph repaints exactly the cells showing it ----- */
    {
        memset(g_screen, ' ', sizeof g_screen);
        memcpy(g_charset, ace_font, sizeof g_charset);
        g_screen[3 * 32 + 5] = 'A';
        g_screen[10 * 32 + 20] = 'A' | 0x80u;
        g_screen[10 * 32 + 25] = 'B';
        g_shadow.valid = false;
        CHECK(present() == ACE_BAND_COUNT, "an invalid shadow should mark every band");
        CHECK(present() == 0, "an unchanged snapshot should mark no band");

        g_charset['A' * 8 + 3] ^= 0xFFu;
        render_band_t bands[ACE_BAND_COUNT];
        unsigned n = render_diff(&g_shadow, g_screen, g_charset, bands);
        CHECK(n == 2, "redefining 'A' should dirty 2 bands, got %u", n);
        CHECK(bands[3].c0 == 5 && bands[3].c1 == 5, "band 3 should be cell 5 alone, got %u..%u",
              bands[3].c0, bands[3].c1);
        CHECK(bands[10].c0 == 20 && bands[10].c1 == 20,
              "band 10 should be the inverse 'A' at 20 alone, got %u..%u",
              bands[10].c0, bands[10].c1);

        glyph_mask_t mask;
        uint8_t was[ACE_CHARSET_BYTES];
        memcpy(was, g_charset, sizeof was);
        was[0x7F * 8] ^= 1u;
        CHECK(render_glyph_mask(g_charset, was, &mask) && mask.w[3] == 0x80000000u &&
              !mask.w[0] && !mask.w[1] && !mask.w[2],
              "glyph $7F changed should set the mask's last bit alone");
    }

    /* ---- randomised: incremental presents match a full render -------- */
    {
        bool stale = run_edits(false, 300);
        CHECK(!stale, "the glyph-change diff left stale pixels");

        /* The control (§13.2): ignoring the character set must miss the
         * charset-only edits, or this test proves nothing. */
        bool control = run_edits(true, 300);
        CHECK(control, "the screen-only control never went stale: the test is blind");
    }

    TEST_DONE();
}
