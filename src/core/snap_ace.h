/* snap_ace.h — .ace snapshots from the archive, imported (design.md §10.5).
 *
 * The format is ACE32's, also written by EightyOne and read by MAME
 * (jupace.cpp, snapshot_cb). It is the address space from $2000 up,
 * run-length encoded: ED 00 ends the file, ED n b is n copies of b
 * (n 1-255; ED itself is always written so), and any other byte is
 * itself. The first 1 KiB, the undisplayed mirror of the screen at
 * $2000, holds the emulator's own state in 32-bit little-endian words:
 * at $2080 the machine's RAMTOP ($4000 3K, $8000 19K, $C000 35K, $0000
 * 51K), and from $2100 the Z80's registers (AF BC DE HL IX IY SP PC AF'
 * BC' DE' HL' IM IFF1 IFF2 I R). Only the low 16 bits of a register
 * pair's word and the low byte of the others' mean anything: the
 * archive's files hold noise above them.
 *
 * What the archive's 199 files show (§16, 2026-10-04): none dumps past
 * $7FFF, whatever its RAMTOP, so a file from a 35K or 51K machine does
 * not hold that machine's top of RAM. Where it held only the return
 * stack the ROM leaves in its key wait, the one word $04F7 at
 * RAMTOP - 2, the import writes that word back (§10.5); any other file
 * whose stack top is not in the file is refused. A run may cross the
 * end of RAM (six archive 3K files end at $8001 with $07 padding), and
 * bytes past the machine's RAM go nowhere.
 *
 * The machine a file needs is its RAMTOP's, and it is refused by any
 * other: a 35K file runs in the 51K machine, there being no 35K one
 * (§6.2). The screen, character set and user RAM come from their own
 * addresses ($2400, $2C00, $3C00, $4000 up); the mirrors' bytes, ACE32's
 * state among them, are not machine state and are not written.
 *
 * Input is through a callback, so the core never sees a file. Loading is
 * two passes, as for .sav: snap_ace_check reads the whole file and says
 * what it needs without touching the machine; only then does
 * snap_ace_load, over the same bytes again, change anything.
 */
#ifndef PICO_ACE_SNAP_ACE_H
#define PICO_ACE_SNAP_ACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ace.h"

/* The key wait the ROM's prompt spins in (ROM $059B: BIT 5,(HL) with
 * HL = FLAGS; $059D: JR Z back), and the one word on the Z80's stack
 * while it does: every archive file taken there whose stack is in the
 * file has $04F7 at RAMTOP - 2 and nothing more (135 files, §16). */
#define SNAP_ACE_WAIT_PC1   0x059Bu
#define SNAP_ACE_WAIT_PC2   0x059Du
#define SNAP_ACE_WAIT_HL    0x3C28u
#define SNAP_ACE_WAIT_RET   0x04F7u

typedef enum {
    SNAP_ACE_OK = 0,
    SNAP_ACE_IO,           /* the callback failed                         */
    SNAP_ACE_NOT_ACE,      /* no end mark, too short or long, or a RAMTOP
                              or IM no .ace has                           */
    SNAP_ACE_OTHER_RAM,    /* a whole file, for another machine            */
    SNAP_ACE_NO_STACK,     /* its stack top is not in the file             */
    SNAP_ACE_BUSY,         /* the CPU is stalled on a tape call            */
} snap_ace_status_t;

/* Read up to max bytes into dst: the count, 0 at the end, -1 on an
 * error. */
typedef int (*snap_ace_read_fn)(void *ctx, uint8_t *dst, size_t max);

typedef struct {
    uint32_t  ramtop;      /* $2080's, $10000 for 51K                     */
    ace_ram_t needs;       /* the machine it loads into                    */
    uint32_t  end;         /* one past the last address the file holds     */
    uint16_t  pc, sp;
    bool      repaired;    /* the key wait's return word written back      */
} snap_ace_info_t;

/* Read the whole file and say what it needs; the machine is not changed.
 * OK means snap_ace_load will load it into m. info is filled as far as
 * the file got, and names the machine for SNAP_ACE_OTHER_RAM. */
snap_ace_status_t snap_ace_check(const ace_t *m, snap_ace_read_fn read, void *ctx,
                                 snap_ace_info_t *info);

/* The same file again, into m: RAM and the CPU as the file has them, the
 * tape request, keys and budget cleared, the field restarted from its
 * first active line, and audio carried on from the machine's clock.
 * Before the first byte of RAM is written the file is checked as far as
 * its header goes; a file that changed between the passes after that
 * leaves a machine that needs a reset. */
snap_ace_status_t snap_ace_load(ace_t *m, snap_ace_read_fn read, void *ctx,
                                snap_ace_info_t *info);

/* "3K", "19K", "35K" or "51K": the machine the file was taken on. */
const char *snap_ace_taken_on(const snap_ace_info_t *info);

const char *snap_ace_status_str(snap_ace_status_t st);

#endif /* PICO_ACE_SNAP_ACE_H */
