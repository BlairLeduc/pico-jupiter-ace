/* keyscript.h — keys for the two tracers of design.md §13.4, by field.
 *
 * Both sides read the same file and hold the same cells down for the
 * same fields, so the keys are not a source of divergence. One key a
 * line, '#' starts a comment:
 *
 *   FIELD HOLD ROW COL [sym|shift]
 *
 * The cell (ROW, half-row 0-7 as A8-A15 selects it, and COL, the bit
 * D0-D4) is down for fields FIELD to FIELD + HOLD - 1, with SYMBOL SHIFT
 * (row 0, D1) or SHIFT (row 0, D0) as well if "sym" or "shift" follows.
 * Field 0 starts at power-on, and every field is 64,896 T at the
 * defaults.
 */
#ifndef PICO_ACE_KEYSCRIPT_H
#define PICO_ACE_KEYSCRIPT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KS_MAX 4096

typedef struct {
    uint32_t field, hold;
    uint8_t  row, col;
    uint8_t  mods;           /* row 0's bits to hold with it */
} ks_key_t;

typedef struct {
    ks_key_t key[KS_MAX];
    int      n;
} keyscript_t;

/* False, with a message on stderr, if the file cannot be read or a line
 * is not a key. */
static inline bool ks_load(keyscript_t *ks, const char *path) {
    ks->n = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "keyscript: cannot open %s\n", path);
        return false;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned field, hold, row, col;
        char mod[8] = "";
        int got = sscanf(line, "%u %u %u %u %7s", &field, &hold, &row, &col, mod);
        if (got <= 0) continue;
        uint8_t mods = got < 5                  ? 0
                     : !strcmp(mod, "sym")   ? 0x02u
                     : !strcmp(mod, "shift") ? 0x01u
                                             : 0xFFu;
        if (got < 4 || row > 7 || col > 4 || mods == 0xFFu || ks->n == KS_MAX) {
            fprintf(stderr, "keyscript: %s:%d: not a key\n", path, lineno);
            fclose(f);
            return false;
        }
        ks->key[ks->n++] = (ks_key_t){ field, hold, (uint8_t)row, (uint8_t)col, mods };
    }
    fclose(f);
    return true;
}

/* The cells down in `field`: one byte per half-row, bit n for Dn. */
static inline void ks_rows(const keyscript_t *ks, uint32_t field, uint8_t rows[8]) {
    memset(rows, 0, 8);
    for (int i = 0; i < ks->n; i++) {
        const ks_key_t *k = &ks->key[i];
        if (field < k->field || field - k->field >= k->hold) continue;
        rows[k->row] |= (uint8_t)(1u << k->col);
        rows[0] |= k->mods;
    }
}

#endif /* PICO_ACE_KEYSCRIPT_H */
