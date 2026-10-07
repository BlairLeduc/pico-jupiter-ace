/* shot.h — a screenshot as a BMP file (design.md §12).
 *
 * The panel has no framebuffer (§7.1), so a screenshot is generated row
 * by row, as a present is, and each row encoded here as it comes. BMP,
 * 24 bits a pixel and uncompressed, because every viewer opens it and it
 * needs neither a buffer of the whole image nor a compressor. Its rows
 * are stored bottom-up: the caller asks the panel for row h-1 first.
 *
 * Portable C, so the host tests encode with the firmware's code. No
 * allocation: the caller owns every buffer.
 */
#ifndef PICO_ACE_SHOT_H
#define PICO_ACE_SHOT_H

#include <stdint.h>

#define SHOT_BMP_HEADER 54u   /* BITMAPFILEHEADER and BITMAPINFOHEADER */

/* The bytes one row takes in the file: 3 a pixel, padded to 4. */
#define SHOT_BMP_ROW(w) ((((w) * 3u) + 3u) & ~3u)

/* The file's names, SHOTnnnn.bmp, from 0001 to SHOT_LAST. */
#define SHOT_LAST 9999u

/* The headers of a w x h image, bottom-up. */
void shot_bmp_header(uint8_t out[SHOT_BMP_HEADER], unsigned w, unsigned h);

/* One row of native RGB565 as the file has it: blue, green, red, each
 * widened to 8 bits so that white is 0xFF, then padding. Returns
 * SHOT_BMP_ROW(w). */
unsigned shot_bmp_row(const uint16_t *px, unsigned w, uint8_t *out);

/* n from a name SHOTnnnn.bmp, in either case, or 0 for any other name. */
unsigned shot_index(const char *name);

#endif /* PICO_ACE_SHOT_H */
