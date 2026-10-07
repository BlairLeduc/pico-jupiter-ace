/* display.c — what core 1 puts on the panel (design.md §7). */

#include "display.h"

#include <string.h>

#include "pico/stdlib.h"

#include "ace_rom.h"
#include "config.h"
#include "font.h"
#include "lcd.h"
#include "render.h"

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3))

_Static_assert(ACE_SCREEN_X + ACE_SCREEN_W <= ACE_PANEL_W &&
               ACE_SCREEN_Y + ACE_SCREEN_H <= ACE_PANEL_H,
               "the guest must fit on the panel");

_Static_assert(ACE_PERF_Y + ACE_GLYPH_ROWS <= ACE_SCREEN_Y,
               "the perf line must sit above the guest");
_Static_assert(ACE_STATUS_Y >= ACE_SCREEN_Y + ACE_SCREEN_H &&
               ACE_STATUS_Y + ACE_GLYPH_ROWS <= ACE_PANEL_H,
               "the status line must sit below the guest");

/* The renderer lives here, on core 1, so its LUT is built by the only
 * core that reads it (§4.3). */
static render_t        s_render;
static render_shadow_t s_shadow;

/* The emulator's pages and lines are in the Ace's own character set,
 * expanded from the embedded ROM (font.h). */
static uint8_t s_font[ACE_CHARSET_BYTES];

/* DMA ping-pong (hardware-notes.md §4.6), the panel's width so that the
 * text lines can use them too. */
static uint16_t s_line[ACE_LINEBUF_COUNT][ACE_LINEBUF_PIXELS];

/* What each line shows now: blank, as lcd_init left the panel. */
typedef struct {
    unsigned y;
    char     text[ACE_TEXT_COLS];
} text_line_t;

static text_line_t s_perf   = { .y = ACE_PERF_Y };
static text_line_t s_status = { .y = ACE_STATUS_Y };

/* Grey, so the lines do not read as the guest's. */
#define TEXT_INK RGB565(0x90, 0x90, 0x90)

void display_init(void) {
    render_init(&s_render, ACE_INK_RGB565, ACE_PAPER_RGB565);
    font_from_rom(ace_rom, s_font);
    memset(s_perf.text, ' ', sizeof s_perf.text);
    memset(s_status.text, ' ', sizeof s_status.text);
    s_shadow.valid = false;
}

const uint8_t *display_font(void) {
    return s_font;
}

void display_invalidate(void) {
    s_shadow.valid = false;
}

void display_present(const uint8_t *screen, const uint8_t *charset, display_stats_t *st) {
    uint32_t t0 = time_us_32();
    display_stats_t s = { .full = !s_shadow.valid };

    render_band_t bands[ACE_BAND_COUNT];
    render_diff(&s_shadow, screen, charset, bands);

    /* The shadow now holds this snapshot, and is what the rows are
     * generated from: the caller's buffer is free to go back to core 0
     * as soon as this returns. */
    const uint8_t *scr = s_shadow.screen, *chr = s_shadow.charset;
    unsigned cur = 0;
    for (unsigned b = 0; b < ACE_BAND_COUNT; b++) {
        unsigned c0 = bands[b].c0, c1 = bands[b].c1;
        if (c0 > c1) continue;
        unsigned w = (c1 - c0 + 1u) * ACE_GLYPH_COLS;
        unsigned y0 = b * ACE_BAND_ROWS;
        lcd_blit_begin(ACE_SCREEN_X + c0 * ACE_GLYPH_COLS, ACE_SCREEN_Y + y0, w, ACE_BAND_ROWS);
        for (unsigned y = y0; y < y0 + ACE_BAND_ROWS; y++) {
            /* The buffer not on the wire: lcd_blit_row waits out the
             * previous row's DMA before starting this one. */
            cur ^= 1u;
            render_cells(&s_render, scr, chr, y, c0, c1, s_line[cur]);
            lcd_blit_row(s_line[cur], w);
        }
        lcd_blit_end();
        s.bands++;
        s.pixels += w * ACE_BAND_ROWS;
    }

    s.us = time_us_32() - t0;
    if (st) *st = s;
}

/* Pixel row r of a text line, the panel's width. */
static void text_row(const text_line_t *l, unsigned r, uint16_t *px) {
    for (unsigned c = 0; c < ACE_TEXT_COLS; c++) {
        uint8_t bits = s_font[(uint8_t)(l->text[c] & 0x7F) * ACE_GLYPH_ROWS + r];
        for (unsigned b = 0; b < ACE_GLYPH_COLS; b++)
            *px++ = (bits & (0x80u >> b)) ? TEXT_INK : ACE_PAPER_RGB565;
    }
}

static void draw_line(text_line_t *l, const char *text) {
    char line[ACE_TEXT_COLS];
    size_t n = strnlen(text, ACE_TEXT_COLS);
    memcpy(line, text, n);
    memset(line + n, ' ', ACE_TEXT_COLS - n);
    if (memcmp(line, l->text, ACE_TEXT_COLS) == 0) return;
    memcpy(l->text, line, ACE_TEXT_COLS);

    unsigned cur = 0;
    lcd_blit_begin(0, l->y, ACE_PANEL_W, ACE_GLYPH_ROWS);
    for (unsigned r = 0; r < ACE_GLYPH_ROWS; r++) {
        cur ^= 1u;
        text_row(l, r, s_line[cur]);
        lcd_blit_row(s_line[cur], ACE_PANEL_W);
    }
    lcd_blit_end();
}

void display_perf(const char *text) {
    draw_line(&s_perf, text);
}

void display_status(const char *text) {
    draw_line(&s_status, text);
}

void display_panel_row(unsigned y, uint16_t *px) {
    for (unsigned x = 0; x < ACE_PANEL_W; x++) px[x] = ACE_PAPER_RGB565;
    if (y >= ACE_SCREEN_Y && y < ACE_SCREEN_Y + ACE_SCREEN_H)
        render_row(&s_render, s_shadow.screen, s_shadow.charset, y - ACE_SCREEN_Y,
                   px + ACE_SCREEN_X);
    else if (y >= s_perf.y && y < s_perf.y + ACE_GLYPH_ROWS)
        text_row(&s_perf, y - s_perf.y, px);
    else if (y >= s_status.y && y < s_status.y + ACE_GLYPH_ROWS)
        text_row(&s_status, y - s_status.y, px);
}

void display_test_pattern(void) {
    const unsigned x = ACE_SCREEN_X, y = ACE_SCREEN_Y;
    const unsigned w = ACE_SCREEN_W, h = ACE_SCREEN_H;
    const unsigned pw = ACE_PANEL_W, ph = ACE_PANEL_H;
    const uint16_t white = RGB565(0xFF, 0xFF, 0xFF);
    const uint16_t grey = RGB565(0x80, 0x80, 0x80);

    lcd_fill(0, 0, pw, ph, 0x0000);

    lcd_fill(0, 0, pw, 1, grey);
    lcd_fill(0, ph - 1u, pw, 1, grey);
    lcd_fill(0, 0, 1, ph, grey);
    lcd_fill(pw - 1u, 0, 1, ph, grey);

    lcd_fill(x, y, w, 1, white);               /* top    */
    lcd_fill(x, y + h - 1u, w, 1, white);      /* bottom */
    lcd_fill(x, y, 1, h, white);               /* left   */
    lcd_fill(x + w - 1u, y, 1, h, white);      /* right  */

    lcd_fill(x + 2u, y + 2u, 16, 16, RGB565(0xFF, 0x00, 0x00));
    lcd_fill(x + w - 18u, y + 2u, 16, 16, RGB565(0x00, 0xFF, 0x00));
    lcd_fill(x + 2u, y + h - 18u, 16, 16, RGB565(0x00, 0x00, 0xFF));
    lcd_fill(x + w - 18u, y + h - 18u, 16, 16, RGB565(0xFF, 0xFF, 0x00));

    memset(s_perf.text, ' ', sizeof s_perf.text);
    memset(s_status.text, ' ', sizeof s_status.text);
    display_invalidate();
}
