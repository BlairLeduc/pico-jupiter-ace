/* core0.h — core 0's loop: the guest (design.md §4.3, §11.1). */
#ifndef PICO_ACE_CORE0_H
#define PICO_ACE_CORE0_H

#include "ace.h"
#include "keymatrix.h"

/* Run the machine a field at a time for ever. Keys first, then the
 * field, then the snapshot, then the field's samples into the audio
 * queue, which blocks while it is full and so paces the guest (EL §6.3).
 * A PICO_ACE_AUDIO=0 build paces on time_us_64() against an absolute
 * field deadline instead. Core 1 must be up, and audio_init done: from
 * here on core 0 logs only through the ring. */
void core0_run(ace_t *m, keymatrix_t *k) __attribute__((noreturn));

#endif /* PICO_ACE_CORE0_H */
