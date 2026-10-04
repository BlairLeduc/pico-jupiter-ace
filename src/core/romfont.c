/* romfont.c — the Ace's character set from its ROM (font.h, design.md §7.5). */

#include <stdbool.h>

#include "font.h"

/* The ROM's own loop at $0055, one instruction a line: for each address
 * L in $2C00-$2CFF, bits of L choose which quarters of the glyph row are
 * ink. */
static uint8_t rrca(uint8_t a, bool *c) {
    *c = a & 1u;
    return (uint8_t)((a >> 1) | (*c ? 0x80u : 0u));
}

void font_from_rom(const uint8_t *rom, uint8_t out[ACE_CHARSET_BYTES]) {
    for (unsigned l = 0; l < 256u; l++) {
        bool c;
        uint8_t a = (uint8_t)(l & 0xBFu);          /* AND $BF       */
        a = rrca(a, &c); a = rrca(a, &c); a = rrca(a, &c);
        if (c) { a = rrca(a, &c); a = rrca(a, &c); }
        a = rrca(a, &c);
        uint8_t b = a;                              /* LD B,A        */
        a = c ? 0xFFu : 0x00u;                      /* SBC A,A       */
        bool c2 = b & 1u;                           /* RR B          */
        b = (uint8_t)((b >> 1) | (c ? 0x80u : 0u));
        b = a;                                      /* LD B,A        */
        a = c2 ? 0xFFu : 0x00u;                     /* SBC A,A       */
        out[l] = (uint8_t)((a & 0xF0u) | (b & 0x0Fu)); /* XOR B : AND $F0 : XOR B */
    }

    /* LDDR from $1FFB, 8 bytes: the copyright sign, glyph 127. */
    unsigned dst = ACE_CHARSET_BYTES - 1u, src = 0x1FFBu;
    for (unsigned i = 0; i < 8u; i++) out[dst--] = rom[src--];

    /* Then glyphs 126 down to 32 ($007A): a blank top row, and seven rows
     * from the table, or six and a blank bottom row when bit 5 of the
     * count is set. */
    for (unsigned n = 0x5Fu; n; n--) {
        unsigned rows = 7u;
        if (n & 0x20u) { out[dst--] = 0; rows = 6u; }
        for (unsigned i = 0; i < rows; i++) out[dst--] = rom[src--];
        out[dst--] = 0;
    }
}
