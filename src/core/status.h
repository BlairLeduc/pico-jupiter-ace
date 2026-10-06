/* status.h — what the status line and the perf line say (design.md §7.4,
 * §12).
 *
 * The status line is one row of text below the guest, for what the
 * Ace's own screen cannot show: the tape. Core 1 never reads guest state
 * while the Z80 runs (§4.3), so what the line needs travels in the
 * snapshot: this struct, a few bytes, filled by core 0 at the end of the
 * field. The tape's name is core 1's already, and it passes it in. The
 * presenter draws the text only when it changes. The perf line is the
 * row above the guest. Both are pico-atom's, in the Ace's mixed case.
 */
#ifndef PICO_ACE_STATUS_H
#define PICO_ACE_STATUS_H

#include <stdint.h>

#include "config.h"

struct ace_s;

typedef enum {
    STATUS_DECK_IDLE = 0,     /* the cassette is empty: the trap's, if any */
    STATUS_DECK_STOP,
    STATUS_DECK_PLAY,
    STATUS_DECK_END,
    STATUS_DECK_REC,
    STATUS_DECK_FULL,         /* recording, and out of room               */
} status_deck_t;

typedef struct {
    uint8_t  deck;            /* status_deck_t                            */
    uint8_t  percent;         /* through the tape; while recording, how
                                 much of the cassette's room it takes     */
    uint8_t  turbo10;         /* guest speed in tenths of real time while
                                 it runs unpaced, 0 when paced; the port's */
    uint16_t errors;          /* blocks the recorder dropped              */
} ace_status_t;

/* The cassette as it is now (cassette.h). The turbo ratio is left 0,
 * for the port to fill. */
void ace_status(const struct ace_s *m, ace_status_t *st);

/* The line's text: ACE_TEXT_COLS characters, space-padded, and a NUL.
 * `tape` is the path in the deck, "" for none, shown without its folder
 * or extension and shortened to fit. With the cassette idle, a tape in
 * the deck is "Tape NAME": the trap reads it whole, and has no place to
 * show. */
void status_format(const ace_status_t *st, const char *tape, char out[ACE_TEXT_COLS + 1]);

/* ---- the perf line --------------------------------------------------------
 * Host counters, not guest state, so not in ace_status_t: core 0 writes
 * these once a second as whole 32-bit words, each single-copy atomic,
 * and core 1 reads them. A read across a write can mix two seconds,
 * which shows for a second and is harmless. */
typedef struct {
    uint32_t busy1000;      /* core 0 outside the pacing wait, thousandths */
    uint32_t head100;       /* times real time it would run unpaced,
                               in hundredths                              */
    uint32_t present_us;    /* the longest present in the second (§7.3) */
    uint32_t dropped;       /* snapshots dropped in the second          */
    uint32_t underruns;     /* underrun samples since boot (§8)         */
    uint32_t late;          /* late refills since boot                  */
} perf_line_t;

/* The perf line's text, ACE_TEXT_COLS characters space-padded:
 * `C0 21% 4.65x  LCD 11.9ms  Drop 0  UR 0 0`. A figure too wide is
 * shown at its widest rather than pushing the rest off. */
void status_perf_format(const perf_line_t *p, char out[ACE_TEXT_COLS + 1]);

/* The status line while the guest is paused, whether or not the line
 * is on. */
void status_paused_format(char out[ACE_TEXT_COLS + 1]);

#endif /* PICO_ACE_STATUS_H */
