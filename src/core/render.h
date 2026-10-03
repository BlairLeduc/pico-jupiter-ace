/* render.h — the row generator and the dirty bands (design.md §7.2, §7.3).
 *
 * The Ace's image is a function of 768 screen bytes and 1,024
 * character-set bytes (§2.5), so there is no framebuffer (§7.1): rows of
 * RGB565 are generated from a snapshot straight into whatever the caller
 * sends to the panel. The generator is portable C, so the host's golden
 * images run the firmware's code.
 *
 * A render_t belongs to the presenter on core 1, never to ace_t (§4.3).
 * Pixels are native RGB565; the panel's byte order is the port's
 * business.
 */
#ifndef PICO_ACE_RENDER_H
#define PICO_ACE_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define ACE_PIXEL_W  (ACE_SCREEN_COLS * 8u)               /* 256 */
#define ACE_PIXEL_H  (ACE_SCREEN_ROWS * ACE_GLYPH_ROWS)   /* 192 */

/* One band per character row, 8 pixel rows tall (§7.3). */
#define ACE_BAND_COUNT  ACE_SCREEN_ROWS
#define ACE_BAND_ROWS   ACE_GLYPH_ROWS

/* The Ace draws white on black (§2.5). */
#define ACE_INK_RGB565    0xFFFFu
#define ACE_PAPER_RGB565  0x0000u

typedef struct {
    /* A glyph row's 8 bits as 8 pixels, bit 7 leftmost: 4 KiB, rebuilt
     * only when the colour pair changes (§7.2). */
    uint16_t lut[256][8];
    uint16_t ink, paper;
} render_t;

void render_init(render_t *r, uint16_t ink, uint16_t paper);

/* Pixel row y (0-191) for cells c0..c1 inclusive: (c1 - c0 + 1) * 8
 * pixels into dst. Bit 7 of a screen byte inverts its cell, and the
 * other seven bits choose the glyph (§7.2). The glyph always comes from
 * `charset`, as the guest wrote it (§7.5). */
void render_cells(const render_t *r, const uint8_t *screen, const uint8_t *charset,
                  unsigned y, unsigned c0, unsigned c1, uint16_t *dst);

/* The whole of pixel row y: ACE_PIXEL_W pixels. */
static inline void render_row(const render_t *r, const uint8_t *screen,
                              const uint8_t *charset, unsigned y, uint16_t *dst) {
    render_cells(r, screen, charset, y, 0, ACE_SCREEN_COLS - 1u, dst);
}

/* ---- Dirty bands (§7.3) ---------------------------------------------- */

/* One bit per glyph, set where its 8 bytes differ between the character
 * set and its shadow. */
typedef struct {
    uint32_t w[ACE_CHARSET_GLYPHS / 32u];
} glyph_mask_t;

/* Step 1: which glyphs changed. True if any did. */
bool render_glyph_mask(const uint8_t *charset, const uint8_t *shadow, glyph_mask_t *mask);

/* Step 2, for one band: the inclusive span of cells whose screen byte
 * changed or whose glyph is in `mask`. False, with *c0 and *c1 untouched,
 * if the band is clean. A zero mask is a screen-only diff, which misses a
 * redefined character; the tests run it as the control that must fail. */
bool render_band_span(const uint8_t *screen, const uint8_t *shadow,
                      const glyph_mask_t *mask, unsigned band,
                      unsigned *c0, unsigned *c1);

/* What the presenter last sent to the panel. `valid` false means the
 * panel's contents are unknown, and the next diff marks everything. */
typedef struct {
    uint8_t screen[ACE_SCREEN_BYTES];
    uint8_t charset[ACE_CHARSET_BYTES];
    bool    valid;
} render_shadow_t;

/* A band's span, cells c0..c1; c0 > c1 when the band is clean. */
typedef struct {
    uint8_t c0, c1;
} render_band_t;

/* Steps 1, 2 and 4: mark every band against the shadow, then copy the
 * snapshot into it. Returns the number of dirty bands. Sending them
 * (step 3) is the caller's. */
unsigned render_diff(render_shadow_t *s, const uint8_t *screen, const uint8_t *charset,
                     render_band_t bands[ACE_BAND_COUNT]);

#endif /* PICO_ACE_RENDER_H */
