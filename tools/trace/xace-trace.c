/* xace-trace.c — xAce's Z80 with no display, printing one line per
 * instruction: the reference half of the trace diff (design.md §13.4).
 *
 *   xace-trace ROM [-f FIELDS] [-k KEYS] [-t TAPE] [-s] [-q]
 *
 * build-xace.sh compiles xAce's z80.c (with xace-hook.h) and tape.c where
 * they stand and links them with this file, which replaces xmain.c: the
 * memory, the port, the interrupt and the clock are this driver's. Each
 * line is ace-trace's format: PC AF BC DE HL IX IY SP T OPCODES.
 *
 * Four things are made to match our machine so that the two keep the
 * same time. None changes what xAce's CPU computes:
 *
 *  - INT is held over the same T-states of each field as
 *    ace_config_default's (the circuit's: lines 248-255 of a field
 *    starting at line 0), where xmain.c latches a wall-clock SIGALRM. It is taken
 *    at most once in that window, since xAce clears no IFF when it takes
 *    one and would otherwise take it again at once; the ROM's handler
 *    outlasts the window (test_field), so ours takes it once too.
 *  - Accepting it costs 13 T, which xAce's loop does not count.
 *  - Its known timing errata are corrected (xace_errata below), each
 *    against the Z80 manual. The count is printed on stderr.
 *  - Keys come from a keyscript, by field, as ace-trace's do.
 *
 * Memory is xmain.c's: 8 KiB of ROM and RAM everywhere above it, with
 * xAce's own mirrors (the store macros in z80.h). -t attaches a .tap
 * through tape.c and patches the ROM as xAce does. -s prints the screen
 * on stderr at the end, as ace-trace's does, and -q prints no trace.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyscript.h"
#include "tape.h"
#include "z80.h"

/* What xAce's z80.c and tape.c expect of xmain.c. */
unsigned char mem[65536];
unsigned char *memptr[8] = {
    mem, mem + 0x2000, mem + 0x4000, mem + 0x6000,
    mem + 0x8000, mem + 0xa000, mem + 0xc000, mem + 0xe000,
};
int memattr[8] = { 0, 1, 1, 1, 1, 1, 1, 1 };
int hsize = 256, vsize = 192;
volatile int interrupted = 0;
int reset_ace = 0;
/* fix_tstates after every instruction: tstates > 0 always. */
unsigned long tstates = 0, tsmax = 0;

/* The field (ace_config_default, design.md §11.1). */
#define FIELD_T  (312u * 208u)
#define INT_RISE (248u * 208u)
#define INT_T    (8u * 208u)

static uint64_t total;          /* T-states since power-on, before this instruction */
static uint64_t stop_t;
static keyscript_t ks;
static unsigned owed;           /* the erratum's T for the instruction running */
static uint64_t taken_in;       /* the field whose INT was taken, plus one */
static unsigned long errata_count[2];
static bool show, quiet;

/* T-states xAce leaves out of the instruction at p (design.md §13.4).
 * Found by the trace diff, 2026-10-03, and read in xAce's source:
 *
 *  0  LD C,n, LD E,n, LD L,n, LD A,n: instr(14,4), (30,4), (46,4), (62,4)
 *     in z80ops.c, where the manual gives 7 T and the odd-register forms
 *     beside them have 7. Prefixed, the same: 11 T, not 8.
 *  1  RES b,(HL) and SET b,(HL): cbops.c adds 4 where the manual's 15 T
 *     (23 indexed) needs 7, as its rotates add.
 */
static unsigned xace_errata(const unsigned char *p) {
    int i = (p[0] == 0xdd || p[0] == 0xfd);
    unsigned char op = p[i];
    if ((op & 0xc7) == 0x06 && (op & 0x08)) {
        errata_count[0]++;
        return 3;
    }
    if (op == 0xcb) {
        unsigned char cb = p[i ? 3 : 1];
        if ((cb & 0xc7) == 0x86 || (cb & 0xc7) == 0xc6) {
            errata_count[1]++;
            return 3;
        }
    }
    return 0;
}

static void screen(void) {
    for (int r = 0; r < 24; r++) {
        char s[33];
        for (int c = 0; c < 32; c++) {
            unsigned char v = mem[0x2400 + r * 32 + c] & 0x7f;
            s[c] = (v >= 0x20 && v < 0x7f && v != 0x60) ? (char)v : '?';
        }
        int n = 32;
        while (n > 0 && s[n - 1] == ' ') n--;
        s[n] = 0;
        fprintf(stderr, "%s\n", s);
    }
}

static void summary(void) {
    if (show) screen();
    fprintf(stderr, "xace-trace: corrected %lu LD r,n and %lu RES/SET (HL) timings\n",
            errata_count[0], errata_count[1]);
}

void xace_trace(unsigned r_pc, unsigned r_af, unsigned r_bc, unsigned r_de, unsigned r_hl,
                unsigned r_ix, unsigned r_iy, unsigned r_sp) {
    if (total >= stop_t) {
        fflush(stdout);
        summary();
        exit(0);
    }
    unsigned char bytes[4];
    for (int k = 0; k < 4; k++) bytes[k] = mem[(r_pc + k) & 0xFFFF];
    owed = xace_errata(bytes);
    if (quiet) return;
    printf("%04X %04X %04X %04X %04X %04X %04X %04X %llu %02X%02X%02X%02X\n", r_pc, r_af, r_bc,
           r_de, r_hl, r_ix, r_iy, r_sp, (unsigned long long)total, mem[r_pc & 0xFFFF],
           mem[(r_pc + 1) & 0xFFFF], mem[(r_pc + 2) & 0xFFFF], mem[(r_pc + 3) & 0xFFFF]);
}

/* After every pass of xAce's loop, before it looks at `interrupted`: INT
 * is up for an instruction that starts inside the window. A prefix is a
 * pass of its own, and the erratum may be paid after it rather than the
 * rest of the instruction; no interrupt is taken between the two. */
void fix_tstates(void) {
    total += tstates + owed;
    tstates = 0;
    owed = 0;
    uint64_t off = total % FIELD_T;
    interrupted = off >= INT_RISE && off < INT_RISE + INT_T && taken_in != total / FIELD_T + 1;
}

void do_interrupt(void) {
    taken_in = total / FIELD_T + 1;
    total += 13;                /* IM 1's acknowledge and RST */
}

/* xmain.c's decode: the keyboard only when one half-row is selected. */
unsigned int in(int h, int l) {
    static const unsigned char row_of[256] = {
        [0xfe] = 1, [0xfd] = 2, [0xfb] = 3, [0xf7] = 4,
        [0xef] = 5, [0xdf] = 6, [0xbf] = 7, [0x7f] = 8,
    };
    if ((l & 0xff) != 0xfe || !row_of[h & 0xff]) return 255;
    uint8_t rows[8];
    ks_rows(&ks, (uint32_t)(total / FIELD_T), rows);
    return (unsigned)(uint8_t)~rows[row_of[h & 0xff] - 1];
}

unsigned int out(int h, int l, int a) {
    (void)h;
    (void)l;
    (void)a;
    return 0;
}

int main(int argc, char **argv) {
    const char *rom = NULL, *tape = NULL;
    unsigned fields = 100;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            fields = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
            if (!ks_load(&ks, argv[++i])) return 2;
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            tape = argv[++i];
        } else if (!strcmp(argv[i], "-s")) {
            show = true;
        } else if (!strcmp(argv[i], "-q")) {
            quiet = true;
        } else if (argv[i][0] != '-' && !rom) {
            rom = argv[i];
        } else {
            fprintf(stderr, "usage: xace-trace ROM [-f FIELDS] [-k KEYS] [-t TAPE] [-s] [-q]\n");
            return 2;
        }
    }
    if (!rom) {
        fprintf(stderr, "xace-trace: no ROM\n");
        return 2;
    }
    FILE *f = fopen(rom, "rb");
    if (!f || fread(mem, 1, 8192, f) != 8192) {
        fprintf(stderr, "xace-trace: cannot read 8,192 bytes from %s\n", rom);
        return 1;
    }
    fclose(f);

    if (tape) {
        tape_patches((char *)mem);
        if (!tape_attach((char *)tape)) {
            fprintf(stderr, "xace-trace: cannot attach %s\n", tape);
            return 1;
        }
    }

    stop_t = (uint64_t)fields * FIELD_T;
    mainloop();
    return 0;
}
