/* guest.h — a real Ace on the host, for the tests that need the ROM
 * (design.md §13.3).
 *
 * The ROM is the one the build embedded (ace_rom.h), which it refused
 * unless the SHA-1 matched, so these tests never skip for want of it.
 *
 * The Ace powers on to a blank screen with the cursor on the bottom line;
 * "OK" appears only after a line is entered. Keys go straight into the
 * matrix here, by cell; M5 replaces that with the firmware's path.
 */
#ifndef PICO_ACE_TEST_GUEST_H
#define PICO_ACE_TEST_GUEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ace.h"

typedef struct {
    ace_t m;
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

void guest_fields(guest_t *g, int n);

/* Hold one matrix cell for 4 fields, with SYMBOL SHIFT if `sym`, then
 * release it for 4. The ROM takes a key on its third consecutive scan
 * (ROM $0310); M5 settles the hold and replaces this with keymatrix. */
void guest_key(guest_t *g, int row, int col, bool sym);

/* Type digits, lower-case letters, space, '+', '.' and '\n' (ENTER) by
 * the cells of design.md §2.4. Anything else is a test bug, and aborts. */
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
