/* test_halt.c — HALT fast-forward on the real ROM (design.md §5.3,
 * §15.2 M12).
 *
 * VLIST halts once a word (ROM $0679), so two 19K machines type the same
 * VLIST, one skipping its HALT repeats and one interpreting them, and
 * must come out the same field by field: registers, RAM, the screen and
 * every PCM sample. The control is the second machine's own count of
 * skipped repeats, which must be zero while the first's is not: a VLIST
 * that never halted would make the comparison prove nothing.
 */
#include <string.h>

#include "guest.h"
#include "test_util.h"

static guest_t a, b;

static bool same_cpu(const z80_t *x0, const z80_t *y0) {
    z80_t x = *x0, y = *y0;
    x.insns = y.insns = x.halts = y.halts = x.run_end = y.run_end = 0;
    x.halt_skip = y.halt_skip = false;
    memset(&x.bus, 0, sizeof x.bus);
    memset(&y.bus, 0, sizeof y.bus);
    return memcmp(&x, &y, sizeof x) == 0;
}

int main(void) {
    CHECK(guest_boot(&a, ACE_RAM_19K, 500), "a: no cursor");
    CHECK(guest_boot(&b, ACE_RAM_19K, 500), "b: no cursor");
    CHECK(a.m.cpu.halt_skip, "skipping is not the default");
    b.m.cpu.halt_skip = false;

    guest_type(&a, "vlist\n");
    guest_type(&b, "vlist\n");

    static int16_t pa[2048], pb[2048];
    int differ = -1;
    for (int f = 0; f < 1500 && differ < 0; f++) {
        guest_fields(&a, 1);
        guest_fields(&b, 1);
        size_t na = ace_audio_drain(&a.m, pa, sizeof pa / sizeof pa[0]);
        size_t nb = ace_audio_drain(&b.m, pb, sizeof pb / sizeof pb[0]);
        if (!same_cpu(&a.m.cpu, &b.m.cpu) || a.m.budget != b.m.budget ||
            a.m.speaker != b.m.speaker || a.m.beeper.edges != b.m.beeper.edges ||
            memcmp(a.m.vram, b.m.vram, sizeof a.m.vram) ||
            memcmp(a.m.cram, b.m.cram, sizeof a.m.cram) ||
            memcmp(a.m.uram, b.m.uram, sizeof a.m.uram) ||
            memcmp(a.m.xram, b.m.xram, sizeof a.m.xram) ||
            na != nb || memcmp(pa, pb, na * sizeof pa[0]))
            differ = f;
    }
    CHECK(differ < 0, "the machines differ after field %d of VLIST", differ);
    CHECK(a.m.cpu.insns + a.m.cpu.halts == b.m.cpu.insns,
          "%u run + %u skipped, against %u run", (unsigned)a.m.cpu.insns,
          (unsigned)a.m.cpu.halts, (unsigned)b.m.cpu.insns);
    CHECK(a.m.cpu.halts > 0 && b.m.cpu.halts == 0, "skipped %u and %u: VLIST did not halt",
          (unsigned)a.m.cpu.halts, (unsigned)b.m.cpu.halts);
    CHECK(guest_screen_has(&a.m, "OK") || strstr(guest_row(&a.m, 22), "OK"),
          "VLIST did not finish:");
    if (test_failures) guest_dump(&a.m, stderr);
    printf("VLIST: %u instructions run and %u HALT repeats skipped, against %u run\n",
           (unsigned)a.m.cpu.insns, (unsigned)a.m.cpu.halts, (unsigned)b.m.cpu.insns);
    TEST_DONE();
}
