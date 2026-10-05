/* ace.h — the guest machine (design.md §4.2, §6).
 *
 * A Z80, the page table that does the Ace's mirroring, the even port and
 * the field. All state is here or sized in config.h; core/ allocates
 * nothing. The renderer and its LUT are not here: the image is a function
 * of ace_screen() and ace_charset(), and belongs to whoever presents it
 * (§4.3, §7.1).
 */
#ifndef PICO_ACE_ACE_H
#define PICO_ACE_ACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "beeper.h"
#include "config.h"
#include "tape.h"
#include "z80.h"

/* The three machines of §6.2, by user RAM. */
typedef enum {
    ACE_RAM_3K,       /* the stock machine: 1 KiB at $3C00, mirrored      */
    ACE_RAM_19K,      /* + 16 KiB at $4000-$7FFF, the default (§18 item 1) */
    ACE_RAM_51K,      /* + 48 KiB at $4000-$FFFF                          */
} ace_ram_t;

/* What the machine is built from. Everything below `rom` is a guest fact
 * that §16 has not settled from a primary source, so it is configuration
 * rather than a #define (EL §14.2); ace_config_default gives the values
 * and says where each came from. */
typedef struct {
    ace_ram_t ram;

    /* ACE_ROM_SIZE bytes, read in place: the page table points into it,
     * so it must outlive the machine. The firmware and the host tests
     * pass ace_rom (ace_rom.h), which the build checked by SHA-1. */
    const uint8_t *rom;

    /* The field (§11.1), in lines of line_t T-states from the start of
     * vertical sync's line count. INT is asserted at the start of line
     * int_line and released int_t T-states later; the display starts at
     * active_line, which is where ace_run_field ends. */
    uint32_t line_t;
    uint32_t field_lines;
    uint32_t int_line;
    uint32_t int_t;
    uint32_t active_line;

    /* What a read returns where nothing drives the bus: character RAM,
     * which the CPU cannot read, and unpopulated addresses (§2.2). */
    uint8_t cram_read;
    uint8_t open_bus;

    /* Serve the ROM's tape block routines from files (tape.h, §10.3).
     * Even when set, the trap stands aside for a ROM whose routines are
     * not the stock ROM's. */
    bool tape_traps;

    /* Skip a HALT's repeats rather than run them (z80_t.halt_skip,
     * §5.3). On by default; off is M12's control. */
    bool halt_skip;
} ace_config_t;

typedef struct ace_s {
    z80_t cpu;

    /* The page table (§6.1). ROM pages read the caller's image in place;
     * every other non-NULL pointer is into this struct, which is why a
     * machine is copied with ace_copy. */
    page_t page[ACE_PAGE_COUNT];

    uint8_t vram[ACE_BLOCK_BYTES];   /* screen at 0, workspace from $300 */
    uint8_t cram[ACE_BLOCK_BYTES];   /* 128 glyphs of 8 rows             */
    uint8_t uram[ACE_BLOCK_BYTES];   /* $3C00, and its mirrors           */
    uint8_t xram[ACE_XRAM_MAX];      /* used up to the configured size   */

    /* The keyboard: one byte per half-row, bit n set while the key on
     * Dn is down. Row r is the one A(8+r) low selects (§2.4). */
    uint8_t keys[ACE_KEY_ROWS];

    /* The tape input as D5 reads it: true is the idle level, a 1. */
    bool tape_in;

    /* The speaker and tape output: an IN from an even port drives it
     * low, an OUT high (§2.3, §8). The beeper turns its edges into PCM,
     * which the port drains once per field with ace_audio_drain, and
     * counts them in beeper.edges. */
    bool     speaker;
    beeper_t beeper;

    /* The tape request the CPU may be stalled on (tape.h). */
    tape_t tape;

    ace_config_t cfg;

    /* The field's three parts in T-states (§11.1), from cfg: the active
     * display up to INT, INT held, and INT released up to the next
     * active line. */
    uint32_t field_t[3];

    /* T-states owed between ace_run calls: the overshoot of the last
     * instruction, paid off from the next slice (§4.2). */
    int32_t budget;

    /* Fields run since ace_init. A counter, not machine state. */
    uint32_t fields;
} ace_t;

/* The 19K machine with the field shape and bus values of §16's current
 * beliefs. cfg->rom is left NULL for the caller to fill. */
void ace_config_default(ace_config_t *cfg);

/* Bytes of user RAM: 1,024, 17,408 or 50,176 (§6.2). */
uint32_t ace_ram_bytes(ace_ram_t ram);

/* "3K", "19K" or "51K", as the machines are known (§6.2). */
const char *ace_ram_name(ace_ram_t ram);

/* T-states in one field: 64,896 at the defaults. */
static inline uint32_t ace_field_t(const ace_t *m) {
    return m->field_t[0] + m->field_t[1] + m->field_t[2];
}

/* Power on: zero RAM (§6.3), build the page table from cfg, and reset the
 * CPU. The field starts at its first active line. False, with the
 * machine untouched, if cfg has no ROM, or a field shape that does not
 * fit in field_lines or whose field is longer than INT32_MAX T-states
 * (the budget is signed). */
bool ace_init(ace_t *m, const ace_config_t *cfg);

/* Power on again with the machine's own configuration, as ace_init with
 * m->cfg, keeping the sample rate the port set and the DC blocker's
 * setting. For a machine whose state can no longer be trusted: a load
 * that failed after it had started to change it (design.md §10.5). */
void ace_power_on(ace_t *m);

/* The CPU's reset line: RAM and the page table are kept, and a tape
 * request the CPU was stalled on is dropped. */
void ace_reset(ace_t *m);

/* Run whole instructions until at least t_states have passed, and return
 * the T-states actually run; the caller carries the overshoot (§4.2).
 * INT stays as it is: only ace_run_field moves it. */
uint32_t ace_run(ace_t *m, uint32_t t_states);

/* One field, from the first active line to the next, split at INT's two
 * edges so that the interrupt is taken at the right instruction and a
 * program that redraws after it has finished by the end (§11.1). Debt
 * carries across the parts and across fields. Returns the T-states run. */
uint32_t ace_run_field(ace_t *m);

/* Copy a machine. Never '=': the page table points into the struct. */
void ace_copy(ace_t *dst, const ace_t *src);

/* After a snapshot has replaced the CPU and RAM (design.md §10.5): a
 * tape request is dropped, the keys are let go (the keyboard's state is
 * now, not then; the port lets go of its held set too), and the beeper
 * carries on from the CPU's clock and the speaker's level. */
void ace_restored(ace_t *m);

/* A key in the matrix, by half-row (0-7, A8-A15) and bit (0-4). Anything
 * outside ACE_KEY_ROWS x ACE_KEY_COLS is ignored. */
void ace_key_set(ace_t *m, int row, int col, bool down);

/* The sample rate is rate_num / rate_den Hz, as a fraction so that the
 * cadence is exact (§8). ace_init starts at the nominal
 * ACE_AUDIO_RATE_NUM / ACE_AUDIO_RATE_DEN; the port passes the rate its
 * clocks actually give. The DC blocker's setting is kept. */
void ace_audio_set_rate(ace_t *m, uint32_t rate_num, uint32_t rate_den);

/* Move up to `max` samples of signed 16-bit mono out of the machine,
 * oldest first. Samples are made as the guest runs, one per 6,656/75 T
 * at the nominal rate; ace_run leaves every sample that ended before its
 * last instruction ready to drain. */
size_t ace_audio_drain(ace_t *m, int16_t *dst, size_t max);

/* The two video inputs (§2.5, §4.4). */
static inline const uint8_t *ace_screen(const ace_t *m)  { return m->vram; }
static inline const uint8_t *ace_charset(const ace_t *m) { return m->cram; }

/* A guest read with no side effects, for tests and dumps: what the CPU
 * would see, through the page table and the slow path. */
uint8_t ace_peek(const ace_t *m, uint16_t addr);

/* A guest write with no time passing, as the CPU's LD would make it:
 * through the page table, and nowhere for ROM and unpopulated pages. */
void ace_poke(ace_t *m, uint16_t addr, uint8_t v);

#endif /* PICO_ACE_ACE_H */
