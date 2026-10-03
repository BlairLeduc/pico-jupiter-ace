/* test_boot.c — the real ROM boots to its prompt in every RAM size and
 * runs a line (design.md §13.3, §15.2 M3). Prints each machine's screen
 * and the T-states from power-on to the prompt. */

#include <string.h>

#include "guest.h"
#include "test_util.h"

static guest_t g;

static const struct {
    ace_ram_t   ram;
    const char *name;
    uint16_t    ramtop;      /* RAMTOP, $3C18, as the ROM sizes it ($0028) */
} machines[] = {
    { ACE_RAM_3K,  "3K",  0x4000u },
    { ACE_RAM_19K, "19K", 0x8000u },
    { ACE_RAM_51K, "51K", 0x0000u },   /* all 64 KiB: the sizing wraps */
};

/* Does any instruction across n fields leave the CPU halted? The run is
 * ace_run_field's, an instruction at a time. */
static bool halts_within(ace_t *m, int n) {
    for (int f = 0; f < n; f++) {
        int32_t budget = m->budget;
        for (int part = 0; part < 3; part++) {
            budget += (int32_t)m->field_t[part];
            while (budget > 0) {
                budget -= (int32_t)ace_run(m, 1);
                if (m->cpu.halted) return true;
            }
            z80_set_int(&m->cpu, part == 0);
        }
        m->budget = budget;
    }
    return false;
}

int main(void) {
    for (size_t i = 0; i < sizeof machines / sizeof machines[0]; i++) {
        const char *name = machines[i].name;
        CHECK(guest_boot(&g, machines[i].ram, 500), "%s: no cursor in 500 fields", name);
        ace_t *m = &g.m;

        printf("Ace %s: prompt after %llu T (%.3f s at 3.25 MHz)\n", name,
               (unsigned long long)g.t_to_prompt, (double)g.t_to_prompt / ACE_CPU_HZ);
        guest_dump(m, stdout);

        /* The power-on screen: blank but for the cursor, at column 1 of
         * the bottom line, the input line's first character cell. */
        for (int r = 0; r < (int)ACE_SCREEN_ROWS - 1; r++)
            CHECK(guest_row(m, r)[0] == 0, "%s: row %d is \"%s\"", name, r, guest_row(m, r));
        const uint8_t *last = ace_screen(m) + 23 * ACE_SCREEN_COLS;
        CHECK(last[1] == GUEST_CURSOR, "%s: $%02X where the cursor should be", name, last[1]);

        CHECK(guest_sysvar16(m, 0x3C18) == machines[i].ramtop,
              "%s: RAMTOP $%04X, expected $%04X", name, guest_sysvar16(m, 0x3C18),
              machines[i].ramtop);
        CHECK(m->cpu.im == 1, "%s: IM %u, expected 1", name, m->cpu.im);

        /* At the prompt the ROM spins on FLAGS bit 5 ($059B) rather than
         * halting, and its key scan's INs never move the speaker (§5.3,
         * §8, §16). */
        uint32_t edges = m->speaker_edges;
        CHECK(!halts_within(m, 50), "%s: the CPU halted at the prompt", name);
        CHECK(m->speaker_edges == edges, "%s: %u speaker edges at the prompt", name,
              m->speaker_edges - edges);

        /* The interpreter runs: a line typed into the matrix comes back
         * with its answer, on the top line, where the ROM moves it. */
        guest_type(&g, "2 2 + .\n");
        guest_fields(&g, 10);
        CHECK(strcmp(guest_row(m, 0), "2 2 + . 4  OK") == 0, "%s: top line \"%s\"", name,
              guest_row(m, 0));
        CHECK(last[1] == GUEST_CURSOR, "%s: no cursor after the line ran", name);
        guest_dump(m, stdout);
    }

    TEST_DONE();
}
