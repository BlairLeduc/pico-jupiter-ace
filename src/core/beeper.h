/* beeper.h — the speaker level to PCM (design.md §8; EL §6.1).
 *
 * The Ace's only sound is the even port's output level, which every IN
 * drives low and every OUT high. A sample is the time average of that
 * level over the guest T-states the sample covers — a box filter at
 * exactly the sample period — not a point sample of it, which aliases
 * audibly. The cost is per edge plus a short loop per sample, and it is
 * exact for square waves of any frequency.
 *
 * Time is kept as a rational, never a truncated rate (EL §6.1, HW §5.2):
 * T-states per sample is num/den, 6,656/75 at 3.25 MHz and 150 MHz, and
 * the sample boundaries are tracked in units of 1/den of a T-state, so
 * they land where they should for ever rather than drifting.
 *
 * Times are the Z80's T counter, which wraps at 2^32 (~22 minutes). Only
 * differences from the current sample's start are taken, and those are
 * never more than a call's worth of T-states, so the wrap does no harm.
 *
 * Output is signed 16-bit mono through a one-pole DC blocker, so a
 * speaker left high and a speaker left low both come to rest at 0, which
 * is the silence value the port pads an underrun with.
 */
#ifndef PICO_ACE_BEEPER_H
#define PICO_ACE_BEEPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Speaker fully high for a whole sample, before the DC blocker. A step
 * through the blocker swings at most this far either side of 0, which
 * leaves int16 a factor of two of headroom. */
#define BEEPER_FULL_SCALE 16384

typedef struct {
    /* T-states per sample = num / den = q + r / den. */
    uint32_t num, den, q, r;
    uint64_t scale;          /* (FULL_SCALE << 32) / num, rounded       */

    /* The current sample began at T `start` plus start_frac / den. */
    uint32_t start;
    uint32_t start_frac;
    uint32_t pos;            /* units (1/den T) of it accounted for     */
    uint32_t high;           /* ...of which the speaker was high        */
    bool     level;

    bool     dc_block;       /* on by default; tests turn it off        */
    int32_t  hp_x;           /* last input                              */
    int32_t  hp_y;           /* last output, Q8                         */

    int16_t  buf[ACE_AUDIO_BUF_LEN];
    uint32_t count;
    uint32_t overflow;       /* samples lost because nobody drained     */
    uint32_t edges;          /* speaker transitions, for the soak (§13.5) */
} beeper_t;

/* Start at T `now` with the speaker at `level`. The sample rate is
 * rate_num / rate_den Hz — on the device clk_sys over (TOP + 1) x
 * oversample x divider, passed as that fraction so nothing is rounded.
 * Zero in any argument means the nominal ACE_AUDIO_RATE at ACE_CPU_HZ. */
void beeper_init(beeper_t *b, uint32_t now, bool level, uint32_t cpu_hz,
                 uint32_t rate_num, uint32_t rate_den);

/* The speaker moved to `level` at T `now`. */
void beeper_set_level(beeper_t *b, uint32_t now, bool level);

/* Emit every sample that ends at or before T `now`. */
void beeper_advance(beeper_t *b, uint32_t now);

/* Move up to `max` samples out, oldest first. Returns how many. */
size_t beeper_drain(beeper_t *b, int16_t *dst, size_t max);

#endif /* PICO_ACE_BEEPER_H */
