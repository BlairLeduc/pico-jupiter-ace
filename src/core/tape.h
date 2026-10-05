/* tape.h — tape phase 1: the ROM's block routines served by a trap
 * (design.md §10.3; EL §8.2).
 *
 * Every tape word (SAVE, BSAVE, LOAD, BLOAD, VERIFY, BVERIFY) ends in one
 * of two ROM routines, each of which moves one block: the 25-byte header
 * or the data. Both take HL = address, DE = length and C = the block's
 * flag byte, $00 for a header and $FF for data; the load takes carry set
 * to load and clear to verify, and returns carry set when the block came
 * in whole with its checksum. On tape a block is the flag, the bytes,
 * and their XOR. The words around them (finding a name, printing it,
 * checking the length) are left to the ROM, which calls again for the
 * next block as it would with a real recorder.
 *
 * When the PC reaches either routine, the CPU stalls there as if WAIT
 * were held and the request waits in ace_t.tape for the port. The port
 * serves it from a .tap file, or declines it, and the ROM's routine then
 * runs as though no trap existed.
 *
 * A served request does not return by itself. The trap puts the machine
 * where the ROM's routine is as it finishes the last byte it would have
 * moved, with the stack it would have left, and lets the ROM run its own
 * last few instructions: the checksum compare, the exit that checks
 * BREAK, re-enables interrupts and leaves the speaker, and the RET. What
 * the ROM leaves is so the ROM's own work, and test_tape holds the rest
 * to the ROM's routine fed its own recorded signal. The trap stands aside
 * unless the ROM's bytes from $1820 to $192C are the stock ROM's.
 *
 * The .tap format (xAce's, and the archive's): each block is a 2-byte
 * little-endian length, then that many bytes, the last being the XOR
 * checksum. The flag byte is not stored. The ROM writes a header and its
 * data as a pair, so a block's flag is taken from its place: even blocks
 * from the start of the file are headers, odd ones data (§16).
 */
#ifndef PICO_ACE_TAPE_H
#define PICO_ACE_TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ace_s;

/* The stock ROM's two block routines (§16, read 2026-10-04). */
#define TAPE_SAVE_PC  0x1820u
#define TAPE_LOAD_PC  0x18A7u

/* The span the trap relies on: both routines and their shared exit. */
#define TAPE_ROM_FIRST 0x1820u
#define TAPE_ROM_END   0x192Du

/* The callers in the stock ROM's tape words: the return address of
 * SAVE's header write ($1A66) and of LOAD's and VERIFY's header read
 * ($1A7E). Both have the header at $2301 (SAVE's to write, LOAD's as
 * asked for) and LOAD's read lands at $231A. */
#define TAPE_SAVE_HEADER_RET 0x1A69u
#define TAPE_LOAD_HEADER_RET 0x1A81u
#define TAPE_HEADER_ASKED    0x2301u

#define TAPE_FLAG_HEADER 0x00u
#define TAPE_FLAG_DATA   0xFFu
#define TAPE_HEADER_LEN  25u     /* type, name, then the lengths and pointers */
#define TAPE_NAME_LEN    10u     /* at offset 1, padded with spaces           */

typedef enum {
    TAPE_NONE = 0,
    TAPE_LOAD,        /* the load routine, carry set */
    TAPE_VERIFY,      /* the load routine, carry clear */
    TAPE_SAVE,
} tape_op_t;

typedef struct {
    tape_op_t op;          /* the request the CPU is stalled on        */
    uint16_t  addr;        /* HL                                        */
    uint16_t  len;         /* DE                                        */
    uint8_t   flag;        /* C: TAPE_FLAG_HEADER or TAPE_FLAG_DATA     */
    uint16_t  ret;         /* the caller's return address, on the stack */

    /* What the trap saw, for the state it hands back. */
    uint16_t  sp, iy;

    /* A load being served. */
    bool      begun;
    bool      flag_ok;     /* the block's flag is the one asked for    */
    bool      stopped;     /* the ROM would stop at byte `at`          */
    uint32_t  at;          /* bytes taken so far, or where it stopped   */
    uint8_t   sum;         /* XOR of the bytes before `at`               */
    uint8_t   last;        /* the byte at `at` once stopped              */

    bool      stock;       /* the ROM's routines are the stock bytes    */
    bool      pass;        /* declined: let the ROM run this call once  */
    uint32_t  served, declined;
} tape_t;

/* The XOR of n bytes: a block's checksum. */
uint8_t tape_checksum(const uint8_t *p, size_t n);

/* The flag of the .tap block at index i from the start of the file. */
static inline uint8_t tape_block_flag(uint32_t i) {
    return (i & 1u) ? TAPE_FLAG_DATA : TAPE_FLAG_HEADER;
}

/* ace.c's: set up at ace_init, and the trap itself, offered the PC by
 * the CPU (z80.h). */
void tape_init(struct ace_s *m);
bool tape_trap(void *ctx);

/* Set the CPU's trap hook for the machine as it stands: the trap when
 * cfg.tape_traps is set or a tape is in the deck, and the exit's cue
 * while the deck runs (cassette.h). The trap stands aside for a ROM that
 * is not the stock one either way. */
void tape_hook(struct ace_s *m);

/* The request the CPU is stalled on, or NULL. */
const tape_t *ace_tape_pending(const struct ace_s *m);

/* No file answers: the ROM's own routine runs, once, as if there were
 * no trap, and waits for a signal on the tape input. */
void ace_tape_decline(struct ace_s *m);

/* Serving TAPE_LOAD or TAPE_VERIFY with one block from the tape: begin
 * with the block's flag, which returns whether its bytes are wanted (a
 * header asked for and data found is a block the ROM skips); then its
 * bytes, the checksum last, in as many pieces as suit the port, until
 * ace_tape_load_wants says no more; then end. A load writes through the
 * page table as the ROM's LD would. */
bool ace_tape_load_begin(struct ace_s *m, uint8_t flag);
void ace_tape_load_data(struct ace_s *m, const uint8_t *src, size_t n);
bool ace_tape_load_wants(const struct ace_s *m);
void ace_tape_load_end(struct ace_s *m);

/* Serving TAPE_SAVE: the block is ace_tape_pending's len bytes at addr,
 * read with ace_peek, and its checksum; once it is on the card, end. */
void ace_tape_save_end(struct ace_s *m);

#endif /* PICO_ACE_TAPE_H */
