/* guest.h — a real Ace on the host, for the tests that need the ROM
 * (design.md §13.3).
 *
 * The ROM is the one the build embedded (ace_rom.h), which it refused
 * unless the SHA-1 matched, so these tests never skip for want of it.
 *
 * The Ace powers on to a blank screen with the cursor on the bottom line;
 * "OK" appears only after a line is entered. Keys reach the matrix the
 * firmware's way: PicoCalc events into keymatrix, replayed once a field
 * (design.md §9.1).
 */
#ifndef PICO_ACE_TEST_GUEST_H
#define PICO_ACE_TEST_GUEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ace.h"
#include "keymatrix.h"

typedef struct {
    ace_t m;
    keymatrix_t k;
    /* T-states from power-on to the instruction that drew the first
     * cursor, or 0 if the boot did not reach it. */
    uint64_t t_to_prompt;
} guest_t;

/* The cursor the ROM draws at the input position: $97, or an inverse C or
 * G in caps or graphics mode (ROM $0282). */
#define GUEST_CURSOR 0x97u

/* The defaults with the embedded ROM and the given RAM size. */
void guest_config(ace_config_t *cfg, ace_ram_t ram);

/* Power on and run until the cursor shows on the bottom line, then 50
 * fields more. False if it did not appear within `max_fields`. */
bool guest_boot(guest_t *g, ace_ram_t ram, int max_fields);

/* n fields, each after keymatrix_field: the held set owns the matrix. */
void guest_fields(guest_t *g, int n);

/* Run fields until everything queued has been replayed and let go, then
 * ACE_KEY_GAP_FIELDS more, so the ROM has seen the last key up. False if
 * that took more than `max_fields`. */
bool guest_settle(guest_t *g, int max_fields);

/* Press and release one PicoCalc code, inside Shift if the PicoCalc
 * types it as a Shift chord, and inside Alt if `alt`; then settle. */
void guest_press(guest_t *g, uint8_t code, bool alt);

/* Type text as a PicoCalc would send it: printable ASCII as itself,
 * '\n' as Enter and '\b' as Backspace. Anything else is a test bug, and
 * aborts. */
void guest_type(guest_t *g, const char *s);

/* One screen row as ASCII, trailing spaces trimmed. Inverse cells read as
 * their character; a code with no ASCII form reads as '?' (`£` is $60 and
 * `©` $7F, §7.5). The buffer is reused. */
const char *guest_row(const ace_t *m, int row);

/* Is there a row reading exactly `text`? */
bool guest_screen_has(const ace_t *m, const char *text);

/* The screen as text: 24 rows inside a frame, with a second marker line
 * under any row that has inverse cells. */
void guest_dump(const ace_t *m, FILE *f);

/* A system variable, as the ROM keeps it at $3C00 + offset. */
static inline uint16_t guest_sysvar16(const ace_t *m, uint16_t addr) {
    return (uint16_t)(ace_peek(m, addr) | (ace_peek(m, (uint16_t)(addr + 1u)) << 8));
}

#endif /* PICO_ACE_TEST_GUEST_H */
