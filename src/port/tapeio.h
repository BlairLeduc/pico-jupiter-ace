/* tapeio.h — .tap files in /ace/tapes/ on the card, serving the tape
 * trap (design.md §10.1, §10.3).
 *
 * The core stalls the CPU on one of the ROM's block routines and leaves
 * the request in ace_t.tape (tape.h); this serves it. Core 1 only, with
 * core 0 parked and the machine core 1's (park.h): card latency can
 * outlast both the field and the audio deadline (hardware-notes.md §7.1).
 *
 * The deck holds one tape, read a block at a time from where it stands,
 * as a recorder would. With the deck empty, LOAD and SAVE use the file
 * the guest names: LOAD SQ plays /ace/tapes/SQ.tap from its start, or
 * failing that the first tape whose first header is SQ's, and SAVE SQ
 * appends to SQ.tap, creating it if need be. A LOAD whose name is a
 * file on the card plays that file whatever is in the deck; any other
 * LOAD reads the deck. When a LOAD reaches the end of the tape it is
 * rewound once, so a program already passed is found; at the end a
 * second time the request is declined, and the ROM waits for a signal
 * as the real machine would, until BREAK.
 *
 * A save is appended to the tape in the deck, or to the named file,
 * through <file>.new and a rename (EL §8.6); a load that finds only the
 * .new, a save cut off between the two, takes it.
 *
 * With fast_tape off (design.md §10.4) the files are found the same way,
 * but the request is declined once the tape is in the cassette whole,
 * so the ROM's own routine reads the signal, or writes it for the
 * recorder; what was recorded is appended to its file at the next park.
 * The cassette plays the tape from where it stands, as a deck would.
 */
#ifndef PICO_ACE_TAPEIO_H
#define PICO_ACE_TAPEIO_H

#include <stdbool.h>
#include <stdint.h>

#include "ace.h"
#include "config.h"

#define TAPEIO_DIR "/ace/tapes"

/* Serve the request the CPU is stalled on, or decline it, and write out
 * what the recorder took: mounts the card, does the job and unmounts.
 * The time taken is in *us. */
void tapeio_serve(ace_t *m, uint32_t *us);

/* The deck, with the card mounted. Insert puts a tape in at its start;
 * NULL or "" empties the deck. NULL, or why not. `m` is NULL before the
 * machine exists (boot_tape). */
const char *tapeio_insert(ace_t *m, const char *path);
const char *tapeio_inserted(void);     /* "" when the deck is empty   */
bool        tapeio_chosen(void);       /* put in by the menu or boot_tape, not found by name */
void        tapeio_rewind(ace_t *m);
uint32_t    tapeio_position(const ace_t *m);  /* the next block, from 0 */

/* With fast_tape off: the cassette started or stopped by hand, for a
 * loader that never calls the ROM. NULL, or why not. */
const char *tapeio_play(ace_t *m, bool on);

/* fast_tape changed: on, the cassette is emptied. */
void        tapeio_mode(ace_t *m);

/* A recording found no card to be written to and is kept; core 0 asks
 * for no park for it until the card changes, which core 1 reports. */
bool        tapeio_flush_waiting(void);
void        tapeio_card_changed(void);

/* The last thing a load or save did that the user should hear about,
 * for the menu's status row; "" for nothing. Cleared by reading. */
const char *tapeio_said(void);

typedef struct {
    char     path[ACE_PATH_MAX];
    char     name[TAPE_NAME_LEN + 1];  /* the first header's, "" if none */
    bool     bytes;                    /* that header's type: bytes, not a dictionary */
    uint32_t size;
} tapeio_entry_t;

/* Up to max .tap files in /ace/tapes/, in directory order. The card
 * must be mounted. */
unsigned tapeio_list(tapeio_entry_t *out, unsigned max);

/* Counters for the heartbeat. */
typedef struct {
    uint32_t loads, saves, declined, errors;
    uint32_t last_us, max_us;
    uint32_t bytes;            /* the last block's */
} tapeio_stats_t;

extern volatile tapeio_stats_t g_tape_stats;

#endif /* PICO_ACE_TAPEIO_H */
