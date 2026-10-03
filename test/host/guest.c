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
    for (int i = 0; i < n; i++) {
        keymatrix_field(&g->k, &g->m);
        ace_run_field(&g->m);
    }
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
    keymatrix_init(&g->k);
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

bool guest_settle(guest_t *g, int max_fields) {
    int f = 0;
    for (; f < max_fields && !keymatrix_idle(&g->k); f++) guest_fields(g, 1);
    guest_fields(g, ACE_KEY_GAP_FIELDS);
    return keymatrix_idle(&g->k);
}

void guest_press(guest_t *g, uint8_t code, bool alt) {
    /* A code that is not its own key's base is a Shift chord on the
     * PicoCalc (keymap_picocalc_canonical). */
    bool shift = keymap_picocalc_canonical(code) != code;
    if (alt)   keymatrix_event(&g->k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
    if (shift) keymatrix_event(&g->k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
    keymatrix_event(&g->k, KEY_EV_PRESSED, code);
    keymatrix_event(&g->k, KEY_EV_RELEASED, code);
    if (shift) keymatrix_event(&g->k, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
    if (alt)   keymatrix_event(&g->k, KEY_EV_RELEASED, PICOCALC_KEY_ALT);
    if (!guest_settle(g, 100)) {
        fprintf(stderr, "guest_press: 0x%02X still held after 100 fields\n", code);
        abort();
    }
}

void guest_type(guest_t *g, const char *s) {
    for (; *s; s++) {
        picocalc_event_t ev[KEYMAP_TEXT_EVENTS];
        unsigned n = keymap_picocalc_text((uint8_t)*s, ev);
        if (n == 0) {
            fprintf(stderr, "guest_type: no PicoCalc key for 0x%02X\n", (uint8_t)*s);
            abort();
        }
        for (unsigned i = 0; i < n; i++) keymatrix_event(&g->k, ev[i].state, ev[i].code);
        if (!guest_settle(g, 100)) {
            fprintf(stderr, "guest_type: 0x%02X still held after 100 fields\n", (uint8_t)*s);
            abort();
        }
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
