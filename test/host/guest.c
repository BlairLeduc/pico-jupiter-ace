/* guest.c — a real Ace on the host (guest.h). */

#include "guest.h"

#include <stdlib.h>
#include <string.h>

#include "ace_rom.h"

void guest_config(ace_config_t *cfg, ace_ram_t ram) {
    ace_config_default(cfg);
    cfg->ram = ram;
    cfg->rom = ace_rom;
}

void guest_fields(guest_t *g, int n) {
    for (int i = 0; i < n; i++) ace_run_field(&g->m);
}

static bool cursor_shown(const ace_t *m) {
    const uint8_t *last = ace_screen(m) + (ACE_SCREEN_ROWS - 1u) * ACE_SCREEN_COLS;
    for (unsigned c = 0; c < ACE_SCREEN_COLS; c++)
        if (last[c] == GUEST_CURSOR) return true;
    return false;
}

/* ace_run_field one instruction at a time: the same budget, the same INT
 * edges, so the same instructions. Stops after the first that leaves the
 * cursor on screen, adding the T-states run to *t. */
static bool field_until_prompt(ace_t *m, uint64_t *t) {
    int32_t budget = m->budget;
    for (int part = 0; part < 3; part++) {
        budget += (int32_t)m->field_t[part];
        while (budget > 0) {
            uint32_t d = ace_run(m, 1);
            budget -= (int32_t)d;
            *t += d;
            if (cursor_shown(m)) return true;
        }
        z80_set_int(&m->cpu, part == 0);
    }
    return false;
}

bool guest_boot(guest_t *g, ace_ram_t ram, int max_fields) {
    ace_config_t cfg;
    guest_config(&cfg, ram);
    g->t_to_prompt = 0;
    if (!ace_init(&g->m, &cfg)) return false;

    /* Field by field until the prompt, then by instruction across the
     * last field to find the one that drew the cursor. */
    static ace_t before, probe;
    uint64_t t = 0;
    for (int f = 0; f < max_fields; f++) {
        ace_copy(&before, &g->m);
        uint64_t t0 = t;
        t += ace_run_field(&g->m);
        if (!cursor_shown(&g->m)) continue;

        ace_copy(&probe, &before);
        t = t0;
        if (!field_until_prompt(&probe, &t)) return false;
        g->t_to_prompt = t;

        guest_fields(g, 50);
        return true;
    }
    return false;
}

void guest_key(guest_t *g, int row, int col, bool sym) {
    if (sym) ace_key_set(&g->m, 0, 1, true);
    ace_key_set(&g->m, row, col, true);
    guest_fields(g, 4);
    ace_key_set(&g->m, row, col, false);
    if (sym) ace_key_set(&g->m, 0, 1, false);
    guest_fields(g, 4);
}

/* The half-rows of design.md §2.4, A8 low first; D0 is the left column. */
static const char *const rows[8] = {
    "\001\002zxc", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " mnbv",
};

void guest_type(guest_t *g, const char *s) {
    for (; *s; s++) {
        char c = *s;
        bool sym = false;
        if (c == '+') { c = 'k'; sym = true; }   /* SYMBOL SHIFT + K */
        if (c == '.') { c = 'm'; sym = true; }   /* SYMBOL SHIFT + M */
        int row = -1, col = -1;
        for (int r = 0; r < 8 && row < 0; r++) {
            const char *p = strchr(rows[r], c);
            if (p && c) { row = r; col = (int)(p - rows[r]); }
        }
        if (row < 0) {
            fprintf(stderr, "guest_type: no cell for '%c'\n", *s);
            abort();
        }
        guest_key(g, row, col, sym);
    }
}

const char *guest_row(const ace_t *m, int row) {
    static char s[ACE_SCREEN_COLS + 1];
    const uint8_t *scr = ace_screen(m) + row * ACE_SCREEN_COLS;
    for (unsigned c = 0; c < ACE_SCREEN_COLS; c++) {
        uint8_t v = scr[c] & 0x7Fu;
        s[c] = (v >= 0x20u && v < 0x7Fu && v != 0x60u) ? (char)v : '?';
    }
    s[ACE_SCREEN_COLS] = 0;
    for (int c = ACE_SCREEN_COLS - 1; c >= 0 && s[c] == ' '; c--) s[c] = 0;
    return s;
}

bool guest_screen_has(const ace_t *m, const char *text) {
    for (int r = 0; r < (int)ACE_SCREEN_ROWS; r++)
        if (strcmp(guest_row(m, r), text) == 0) return true;
    return false;
}

void guest_dump(const ace_t *m, FILE *f) {
    fprintf(f, "+--------------------------------+\n");
    for (int r = 0; r < (int)ACE_SCREEN_ROWS; r++) {
        fprintf(f, "|%-32s|\n", guest_row(m, r));
        const uint8_t *scr = ace_screen(m) + r * ACE_SCREEN_COLS;
        char inv[ACE_SCREEN_COLS + 1];
        bool any = false;
        for (unsigned c = 0; c < ACE_SCREEN_COLS; c++) {
            inv[c] = (scr[c] & 0x80u) ? '^' : ' ';
            any |= (scr[c] & 0x80u) != 0;
        }
        inv[ACE_SCREEN_COLS] = 0;
        if (any) fprintf(f, " %s  inverse\n", inv);
    }
    fprintf(f, "+--------------------------------+\n");
}
