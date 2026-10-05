/* ace-trace.c — this project's core on the host, printing one line per
 * instruction: our half of the trace diff (design.md §13.4).
 *
 *   ace-trace [-r 3k|19k|51k] [-f FIELDS] [-k KEYS] [-s]
 *
 * Each line is the state before an instruction, as xace-trace prints it:
 *
 *   PC AF BC DE HL IX IY SP T OPCODES
 *
 * in hex but T, which is decimal T-states since power-on, and OPCODES the
 * four bytes at PC. An interrupt's acceptance is a step of its own in
 * z80_step but not an instruction, so it has no line; the handler's first
 * instruction does, with the 13 T of acknowledge in its T.
 *
 * The machine is the 51K by default, since xAce's memory is all RAM above
 * the ROM, and it runs as ace_run_field runs it (§11.1), one instruction
 * at a time. Keys change between fields (keyscript.h). -s prints the
 * screen on stderr at the end.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "keyscript.h"

static ace_t m;
static keyscript_t ks;
static uint64_t total;

static bool accepts_interrupt(const z80_t *c) {
    return (c->nmi_pending && !c->prefix) || (c->int_line && c->iff1 && !c->int_blocked);
}

static void line(const ace_t *a) {
    const z80_t *c = &a->cpu;
    printf("%04X %04X %04X %04X %04X %04X %04X %04X %llu %02X%02X%02X%02X\n", c->pc, c->af.w,
           c->bc.w, c->de.w, c->hl.w, c->ix.w, c->iy.w, c->sp, (unsigned long long)total,
           ace_peek(a, c->pc), ace_peek(a, (uint16_t)(c->pc + 1u)),
           ace_peek(a, (uint16_t)(c->pc + 2u)), ace_peek(a, (uint16_t)(c->pc + 3u)));
}

/* ace_run_field, an instruction at a time (test/host/guest.c). */
static void field(ace_t *a) {
    for (int part = 0; part < 3; part++) {
        a->budget += (int32_t)a->field_t[part];
        while (a->budget > 0) {
            if (!accepts_interrupt(&a->cpu)) line(a);
            uint32_t d = ace_run(a, 1);
            a->budget -= (int32_t)d;
            total += d;
        }
        z80_set_int(&a->cpu, part == 0);
    }
    a->fields++;
}

static void screen(const ace_t *a) {
    for (unsigned r = 0; r < ACE_SCREEN_ROWS; r++) {
        char s[ACE_SCREEN_COLS + 1];
        for (unsigned c = 0; c < ACE_SCREEN_COLS; c++) {
            uint8_t v = ace_screen(a)[r * ACE_SCREEN_COLS + c] & 0x7Fu;
            s[c] = (v >= 0x20u && v < 0x7Fu && v != 0x60u) ? (char)v : '?';
        }
        int n = ACE_SCREEN_COLS;
        while (n > 0 && s[n - 1] == ' ') n--;
        s[n] = 0;
        fprintf(stderr, "%s\n", s);
    }
}

int main(int argc, char **argv) {
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    cfg.ram = ACE_RAM_51K;
    cfg.wait_states = false;   /* xAce holds nothing (design.md §6.4) */
    unsigned fields = 100;
    bool show = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            const char *r = argv[++i];
            cfg.ram = !strcmp(r, "3k") ? ACE_RAM_3K : !strcmp(r, "19k") ? ACE_RAM_19K : ACE_RAM_51K;
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            fields = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
            if (!ks_load(&ks, argv[++i])) return 2;
        } else if (!strcmp(argv[i], "-s")) {
            show = true;
        } else {
            fprintf(stderr, "usage: ace-trace [-r 3k|19k|51k] [-f FIELDS] [-k KEYS] [-s]\n");
            return 2;
        }
    }

    if (!ace_init(&m, &cfg)) return 1;
    for (unsigned f = 0; f < fields; f++) {
        uint8_t rows[8];
        ks_rows(&ks, f, rows);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 5; c++) ace_key_set(&m, r, c, (rows[r] >> c) & 1u);
        field(&m);
    }
    if (show) screen(&m);
    return 0;
}
