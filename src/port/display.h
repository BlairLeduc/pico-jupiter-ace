/* display.h — what core 1 puts on the panel (design.md §7).
 *
 * Core 1 only. Owns the renderer and its LUT, the presented shadow and
 * the DMA line buffers (§4.3). There is no framebuffer (§7.1): every
 * pixel sent is generated from a snapshot.
 */
#ifndef PICO_ACE_DISPLAY_H
#define PICO_ACE_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t us;        /* wall time of the present                    */
    uint16_t bands;     /* bands sent (24 for a full redraw)           */
    uint32_t pixels;    /* pixels on the wire                          */
    bool     full;      /* the shadow was invalid: everything was sent */
} display_stats_t;

void display_init(void);

/* Present one snapshot: diff its screen and character set against the
 * shadow (§7.3), send each dirty band's span as one 8-row window at
 * (32, 64) (§7.4), and make the snapshot the shadow. */
void display_present(const uint8_t *screen, const uint8_t *charset, display_stats_t *st);

/* The character set the emulator's pages use: the Ace's own, from its
 * ROM (font.h), so that a page reads as the guest's screen does. */
const uint8_t *display_font(void);

/* Forget what is on the panel, so the next present sends everything. */
void display_invalidate(void);

/* The perf line at the panel's top and the status line at its foot
 * (§7.4, §12), where pico-atom has them: ACE_TEXT_COLS characters of the
 * Ace's own font, each drawn only when its text differs from what is
 * there. Shorter text is padded with spaces. */
void display_perf(const char *text);
void display_status(const char *text);

/* The bring-up pattern (design.md §15.2 M6): a 1-px white border exactly
 * on the guest's 256x192 rectangle at (32,64), with a 16x16 block in each
 * inside corner (red top-left, green top-right, blue bottom-left, yellow
 * bottom-right), so a mirrored axis or swapped R/B shows as the wrong
 * colour in the wrong corner. A 1-px grey frame on the panel's own edge
 * shows that all 320x320 are addressed. Everything else is black. Also
 * invalidates, since it overwrites the guest's rectangle. */
void display_test_pattern(void);

#endif /* PICO_ACE_DISPLAY_H */
