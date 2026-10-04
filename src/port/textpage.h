/* textpage.h — a page of text for the screens the emulator draws itself,
 * the menu and pause (design.md §12).
 *
 * The page is 768 screen bytes, as the guest's are, drawn with the
 * emulator's own font (font.h) through the same row generator, so it
 * costs no new drawing code and closing it is display_invalidate()
 * (§7.5, EL §10). Codes are ASCII; bit 7 is inverse video, as on the Ace.
 */
#ifndef PICO_ACE_TEXTPAGE_H
#define PICO_ACE_TEXTPAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define TEXT_COLS ((int)ACE_SCREEN_COLS)
#define TEXT_ROWS ((int)ACE_SCREEN_ROWS)

void textpage_clear(uint8_t *scr);
void textpage_put(uint8_t *scr, int row, int col, const char *s, bool inverse);

/* A whole row: the text, then blanks to the edge, all in one video. */
void textpage_line(uint8_t *scr, int row, const char *s, bool inverse);

#endif /* PICO_ACE_TEXTPAGE_H */
