/* textpage.c — a page of text (textpage.h). */

#include "textpage.h"

#include <string.h>

/* Printable ASCII is the font's own order (font.h); anything else is a
 * blank. */
static uint8_t glyph(char c, bool inverse) {
    uint8_t a = (uint8_t)c;
    if (a < 0x20u || a > 0x7Eu) a = ' ';
    return inverse ? (uint8_t)(a | 0x80u) : a;
}

void textpage_clear(uint8_t *scr) {
    memset(scr, ' ', ACE_SCREEN_BYTES);
}

void textpage_put(uint8_t *scr, int row, int col, const char *s, bool inverse) {
    if (row < 0 || row >= TEXT_ROWS) return;
    for (; *s && col < TEXT_COLS; s++, col++)
        if (col >= 0) scr[row * TEXT_COLS + col] = glyph(*s, inverse);
}

void textpage_line(uint8_t *scr, int row, const char *s, bool inverse) {
    if (row < 0 || row >= TEXT_ROWS) return;
    for (int col = 0; col < TEXT_COLS; col++) {
        char c = *s ? *s++ : ' ';
        scr[row * TEXT_COLS + col] = glyph(c, inverse);
    }
}
