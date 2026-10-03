/* render.c — the row generator and the dirty bands (design.md §7.2, §7.3). */

#include "render.h"

#include <string.h>

void render_init(render_t *r, uint16_t ink, uint16_t paper) {
    r->ink = ink;
    r->paper = paper;
    for (unsigned b = 0; b < 256u; b++)
        for (unsigned x = 0; x < 8u; x++)
            r->lut[b][x] = (b & (0x80u >> x)) ? ink : paper;
}

void render_cells(const render_t *r, const uint8_t *screen, const uint8_t *charset,
                  unsigned y, unsigned c0, unsigned c1, uint16_t *dst) {
    const uint8_t *cell = screen + (y / ACE_GLYPH_ROWS) * ACE_SCREEN_COLS;
    const uint8_t *rows = charset + (y % ACE_GLYPH_ROWS);
    for (unsigned c = c0; c <= c1; c++) {
        uint8_t code = cell[c];
        uint8_t bits = rows[(code & 0x7Fu) * ACE_GLYPH_ROWS];
        if (code & 0x80u) bits = (uint8_t)~bits;

        /* Eight stores, not memcpy: at 16 bytes the call is the cost
         * (hardware-notes.md §9.4). */
        const uint16_t *p = r->lut[bits];
        dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = p[3];
        dst[4] = p[4]; dst[5] = p[5]; dst[6] = p[6]; dst[7] = p[7];
        dst += 8;
    }
}

bool render_glyph_mask(const uint8_t *charset, const uint8_t *shadow, glyph_mask_t *mask) {
    bool any = false;
    memset(mask, 0, sizeof *mask);
    for (unsigned g = 0; g < ACE_CHARSET_GLYPHS; g++) {
        if (memcmp(charset + g * ACE_GLYPH_ROWS, shadow + g * ACE_GLYPH_ROWS,
                   ACE_GLYPH_ROWS) != 0) {
            mask->w[g / 32u] |= 1u << (g % 32u);
            any = true;
        }
    }
    return any;
}

bool render_band_span(const uint8_t *screen, const uint8_t *shadow,
                      const glyph_mask_t *mask, unsigned band,
                      unsigned *c0, unsigned *c1) {
    const uint8_t *now = screen + band * ACE_SCREEN_COLS;
    const uint8_t *was = shadow + band * ACE_SCREEN_COLS;
    int lo = -1, hi = -1;
    for (unsigned c = 0; c < ACE_SCREEN_COLS; c++) {
        unsigned g = now[c] & 0x7Fu;
        if (now[c] != was[c] || (mask->w[g / 32u] & (1u << (g % 32u)))) {
            if (lo < 0) lo = (int)c;
            hi = (int)c;
        }
    }
    if (lo < 0) return false;
    *c0 = (unsigned)lo;
    *c1 = (unsigned)hi;
    return true;
}

unsigned render_diff(render_shadow_t *s, const uint8_t *screen, const uint8_t *charset,
                     render_band_t bands[ACE_BAND_COUNT]) {
    glyph_mask_t mask;
    render_glyph_mask(charset, s->charset, &mask);

    unsigned dirty = 0;
    for (unsigned b = 0; b < ACE_BAND_COUNT; b++) {
        unsigned c0 = 1, c1 = 0;
        if (!s->valid) {
            c0 = 0;
            c1 = ACE_SCREEN_COLS - 1u;
        } else {
            render_band_span(screen, s->screen, &mask, b, &c0, &c1);
        }
        bands[b].c0 = (uint8_t)c0;
        bands[b].c1 = (uint8_t)c1;
        if (c0 <= c1) dirty++;
    }

    memcpy(s->screen, screen, ACE_SCREEN_BYTES);
    memcpy(s->charset, charset, ACE_CHARSET_BYTES);
    s->valid = true;
    return dirty;
}
