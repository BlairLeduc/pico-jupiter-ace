/* font.h — the emulator's own font (design.md §7.5).
 *
 * A character set for the emulator's pages (menu, About), laid out as the
 * guest's is: 128 glyphs of 8 rows, bit 7 leftmost, in ASCII order. A page
 * fills its own 768-byte screen and passes this as the character set, so
 * it goes through the same row generator and never depends on what a
 * program has done to character RAM.
 */
#ifndef PICO_ACE_FONT_H
#define PICO_ACE_FONT_H

#include <stdint.h>

#include "config.h"

extern const uint8_t ace_font[ACE_CHARSET_BYTES];

/* The Ace's own character set, as its ROM writes it into character RAM at
 * power-on ($0052-$008D), expanded from the ROM image: the 32 block
 * graphics worked out from their codes, the 95 printable glyphs from the
 * table read down from $1FF3, and the copyright sign from $1FF4. The
 * emulator's pages are drawn with it, so they read as the Ace does
 * without depending on what a program has done to character RAM.
 * test_render holds it to what the ROM itself writes. */
void font_from_rom(const uint8_t *rom, uint8_t out[ACE_CHARSET_BYTES]);

#endif /* PICO_ACE_FONT_H */
