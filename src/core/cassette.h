/* cassette.h — tape phase 2: the signal (design.md §10.4; EL §4.3, §8.3).
 *
 * A .tap image plays into the tape input, D5 of an even-port IN, as the
 * square wave the ROM's own SAVE would have written for it, clocked in
 * T-states. Whatever reads it, the ROM's block routine at $18A7 or a
 * loader of a program's own, sees what it would see from a recorder.
 * Nothing is clocked per instruction: the input is brought up to date
 * when the port is read, the only time it can be seen, and a read before
 * the next edge costs a subtract and a branch (ace.c).
 *
 * Every half-cycle is the count of the ROM's save routine at $1820 for
 * that place in the block (cassette.c names the instructions), so the
 * signal played is the one the ROM records, edge for edge, and
 * test_cassette holds it to that. Blocks are separated by the gap SAVE
 * leaves between a header and its data.
 *
 * The recorder is the other half: D3 of every even-port OUT, the line
 * the save routine drives (§16), read back as half-cycles and decoded as
 * the ROM's load routine would, into .tap blocks appended to the image,
 * which is whole after every byte.
 *
 * The deck has no motor control, as the Ace has none. It follows the
 * ROM's cues instead (tape.c): the load routine's entry starts the
 * player and the save routine's entry the recorder, if the deck is
 * recording; the routines' shared exit at $1892 stops both. The player
 * can also be started and stopped by hand, for a loader that never calls
 * the ROM.
 *
 * The image is the caller's buffer and must outlive the insertion.
 */
#ifndef PICO_ACE_CASSETTE_H
#define PICO_ACE_CASSETTE_H

#include <stdbool.h>
#include <stdint.h>

struct ace_s;

/* Silence between two blocks, in T-states: with the next leader's first
 * half, the 4,204 T that SAVE leaves between its header and its data
 * (measured by test_cassette, design.md §10.4). */
#define CASSETTE_GAP_T 2193u

typedef struct {
    bool     armed;        /* the deck is recording: a save cue starts a block */
    bool     on;           /* between the save routine's entry and its exit    */
    bool     level;        /* D3 as the last edge left it                      */
    uint32_t last;         /* T of that edge                                   */
    uint8_t  state;        /* seeking a leader, the sync, the bits             */
    uint32_t lead;         /* leader half-cycles in a row                      */
    uint32_t half;         /* a bit's first half, 0 before it                  */
    uint8_t  byte, bits;   /* the byte being read, and its bits so far         */
    bool     open;         /* a block is being written                         */
    uint32_t at;           /* its length field's offset in the image           */
    uint32_t n;            /* its bytes written, the flag not among them       */
    uint32_t place;        /* blocks in the image before it: its flag (tape.h) */
    uint32_t mark;         /* the image's length when last written out          */
    bool     full;         /* ran out of room, and records nothing more        */
    uint32_t blocks;       /* blocks recorded                                  */
    uint32_t errors;       /* blocks dropped: a flag out of its place          */
} cassette_rec_t;

typedef struct {
    uint8_t *img;          /* a .tap image, `len` bytes of room `cap`         */
    uint32_t len, cap;
    bool     loaded;

    bool     playing;
    bool     ended;        /* played to the end since the last rewind         */
    bool     level;        /* the line as the save routine drives it          */
    bool     toggles;      /* the next change flips the line; a gap's does not */
    uint32_t next;         /* T of the next change, while playing             */
    uint32_t left;         /* next - now, while stopped                       */

    /* Where the walk is: the block's offset and index, its bytes as the
     * length field gives them (the checksum included) and as many as the
     * image holds, the phase within it and the count within the phase. */
    uint32_t pos, index, n, avail;
    uint8_t  phase;
    uint32_t count;
    uint32_t edges;        /* changes played, for the heartbeat               */

    cassette_rec_t rec;
} cassette_t;

/* ---- the deck, on its own ------------------------------------------------ */

/* Bring the player up to `now`. ace.c calls it when the port is read at
 * or after `next`. */
void cassette_advance(cassette_t *c, uint32_t now);

/* D3 changed to `level` at `now`, while recording (ace.c). */
void cassette_rec_edge(cassette_t *c, uint32_t now, bool level);

/* The half-cycles of the image in order, for tests: each call gives the
 * next one's length in T and whether it ends in a change of level. The
 * walk is the player's own; false at the end of the image. Rewinds the
 * deck first when `from_start`. */
bool cassette_walk(cassette_t *c, bool from_start, uint32_t *t, bool *toggles);

/* ---- the deck in the machine ---------------------------------------------- */

/* Put an image in, rewound and stopped. `cap` is the room the recorder
 * may grow it to; cap == len is a tape that takes no recording. */
void ace_cassette_insert(struct ace_s *m, uint8_t *img, uint32_t len, uint32_t cap);
void ace_cassette_eject(struct ace_s *m);

/* By hand, for a loader that never calls the ROM. Starting at the end of
 * the tape does nothing: rewind first, as on a deck. */
void ace_cassette_play(struct ace_s *m, bool on);
void ace_cassette_rewind(struct ace_s *m);

/* Recording: armed, the save routine's entry starts a block and its exit
 * ends it, appended to the image. Disarming ends one in progress. */
void ace_cassette_record(struct ace_s *m, bool armed);

/* Playing or recording now: the guest may run unpaced (§11.2). */
bool ace_cassette_running(const struct ace_s *m);

/* The recorded bytes not yet written out, [*from, *to) of the image; false
 * when there are none or a block is still being recorded. */
bool ace_cassette_unsaved(const struct ace_s *m, uint32_t *from, uint32_t *to);

/* They are on the card: or, with keep false, drop them from the image,
 * for a recording that belongs to another file than the deck's. */
void ace_cassette_saved(struct ace_s *m, bool keep);

/* The cues, from the trap (tape.c). */
void cassette_cue_load(struct ace_s *m);
void cassette_cue_save(struct ace_s *m);
void cassette_cue_exit(struct ace_s *m);

/* The machine was reset or restored: stop, keeping the place. */
void cassette_stop_all(struct ace_s *m);

#endif /* PICO_ACE_CASSETTE_H */
