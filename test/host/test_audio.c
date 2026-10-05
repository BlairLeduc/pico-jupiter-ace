/* test_audio.c — the speaker into PCM (design.md §8, §15.2 M8).
 *
 * The tone is the ROM's own BEEP ( m n -- ), whose loop at $0BAF is
 * counted by hand below, and the samples are checked two ways: against
 * an independent box filter built from the edges the guest actually
 * made, and by measuring the pitch of the output against the count.
 *
 * BEEP, read from the ROM 2026-10-04: m arrives in BC, the delay is
 * HL = m + 249 with its low byte then incremented (INC L, no carry), and
 * interrupts are off for the whole note.
 *
 *   $0BAF  LD A,$7F         7     IN half-row $7F: SPACE stops the note
 *   $0BB1  IN A,($FE)      11     speaker low
 *          RRCA             4
 *          JR NC,$0BC7      7     not taken while SPACE is up
 *          CALL $0BC9      17+S
 *          DEC DE           6
 *          LD A,D           4
 *   $0BBB  OUT ($FE),A     11     speaker high
 *          CALL $0BC9      17+S
 *          OR E             4
 *          JP NZ,$0BAF     10
 *
 * IN to OUT and OUT to the next IN are both 49 + S. The delay $0BC9 is
 * LD B,L; LD C,H; then DJNZ rounds of L, and of 256 for each further
 * unit of H, with DEC B; DEC C; JP NZ between them, and RET:
 * S = 8 + 13(L + 1) + 3,328(H - 1) + 10 = 13 HL - 3,297 = 13m - 47.
 * Half period 13m + 2 T, period 26m + 4 T: 8m us and 4 T at 3.25 MHz,
 * as the manual says. It holds for m >= 6 with (m + 249) & $FF != $FF.
 */

#include <math.h>
#include <string.h>

#include "guest.h"
#include "test_util.h"

static guest_t g;

#define HALF(m)    (13u * (m) + 2u)
#define MAX_SAMPLES 80000u
#define MAX_EDGES   4000u

static int16_t  samples[MAX_SAMPLES];
static uint32_t edges[MAX_EDGES];   /* T at which each moving instruction began */

/* Start the beeper afresh at the nominal rate, discarding whatever the
 * fields before left in it, with the DC blocker as asked. */
static void restart_beeper(ace_t *m, bool dc_block) {
    m->beeper.dc_block = dc_block;
    ace_audio_set_rate(m, ACE_AUDIO_RATE_NUM, ACE_AUDIO_RATE_DEN);
}

/* n fields the port's way: keys, the field, then a drain. */
static size_t fields_draining(guest_t *gg, unsigned n, int16_t *dst, size_t max) {
    size_t ns = 0;
    int16_t junk[ACE_AUDIO_BUF_LEN];
    for (unsigned f = 0; f < n; f++) {
        guest_fields(gg, 1);
        if (dst) ns += ace_audio_drain(&gg->m, dst + ns, max - ns);
        else ns += ace_audio_drain(&gg->m, junk, ACE_AUDIO_BUF_LEN);
    }
    return ns;
}

/* Type "m n BEEP" and come back with the note sounding: the typing's
 * replay takes a few fields, and the ROM a few more to redraw the line
 * through the waiting mirror (§6.4), so n must outlast them. */
static bool start_beep(unsigned m, unsigned n) {
    if (!guest_boot(&g, ACE_RAM_19K, 500)) return false;
    char line[40];
    snprintf(line, sizeof line, "%u %u BEEP\n", m, n);
    guest_type(&g, line);
    for (int f = 0; f < 10 && g.m.cpu.iff1; f++) guest_fields(&g, 1);
    return !g.m.cpu.iff1;     /* DI for the whole note */
}

int main(void) {
    /* ---- the rate is kept as an exact fraction (§8) -------------------- */
    {
        CHECK(guest_boot(&g, ACE_RAM_19K, 500), "no prompt");
        ace_t *m = &g.m;
        CHECK(m->beeper.num == 6656u && m->beeper.den == 75u,
              "3.25 MHz at 150 MHz / 4096 should be 6656/75 T a sample, got %u/%u",
              m->beeper.num, m->beeper.den);
        CHECK(ace_field_t(m) == 64896u, "a field of %u T", ace_field_t(m));

        /* A field is 731.25 samples, so four make 2,925 exactly, and at
         * any point the count is the samples that have ended by the last
         * T run. */
        restart_beeper(m, true);
        uint32_t t0 = m->beeper.start;
        size_t total = fields_draining(&g, 4, NULL, 0);
        uint64_t ran = (uint32_t)(m->cpu.t - t0);
        CHECK(total == ran * 75u / 6656u, "%llu T should make %llu samples, made %zu",
              (unsigned long long)ran, (unsigned long long)(ran * 75u / 6656u), total);
        total += fields_draining(&g, 3000, NULL, 0);
        ran = (uint32_t)(m->cpu.t - t0);
        CHECK(total == ran * 75u / 6656u, "%llu T should make %llu samples, made %zu",
              (unsigned long long)ran, (unsigned long long)(ran * 75u / 6656u), total);
        CHECK(m->beeper.overflow == 0, "overflow %u", m->beeper.overflow);

        /* The prompt is silent: the key scan's INs hold the speaker low
         * and nothing OUTs (§8), so the ROM does not click at idle. */
        uint32_t e = m->beeper.edges;
        size_t ns = fields_draining(&g, 50, samples, MAX_SAMPLES);
        int nonzero = 0;
        for (size_t k = 0; k < ns; k++) nonzero += samples[k] != 0;
        CHECK(m->beeper.edges == e, "%u edges at the prompt", m->beeper.edges - e);
        CHECK(nonzero == 0, "%d nonzero samples at the prompt", nonzero);

        /* Nor does typing: a key taken makes no edge. */
        e = m->beeper.edges;
        guest_type(&g, "1");
        printf("prompt: %u edges typing a key\n", m->beeper.edges - e);
        CHECK(m->beeper.edges == e, "%u edges typing a key", m->beeper.edges - e);
    }

    /* ---- BEEP's period is the hand count, at three pitches ------------- *
     * Step an instruction at a time, note where the speaker moved, and
     * rebuild every sample independently in double precision. */
    static const unsigned pitches[] = { 50u, 100u, 300u };
    for (size_t p = 0; p < sizeof pitches / sizeof pitches[0]; p++) {
        unsigned mm = pitches[p];
        CHECK(start_beep(mm, 1000u), "m=%u: BEEP did not start", mm);
        ace_t *m = &g.m;
        restart_beeper(m, false);
        uint32_t t0 = m->beeper.start;
        bool lvl = m->speaker;

        unsigned ne = 0;
        size_t ns = 0;
        bool was = m->speaker;
        while ((uint32_t)(m->cpu.t - t0) < 400000u) {
            uint32_t at = m->cpu.t;
            ace_run(m, 1);
            if (m->speaker != was) {
                was = !was;
                if (ne < MAX_EDGES) edges[ne++] = at;
            }
            ns += ace_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        CHECK(ne > 50, "m=%u: %u edges", mm, ne);

        unsigned off = 0;
        for (unsigned i = 1; i < ne; i++)
            if (edges[i] - edges[i - 1] != HALF(mm)) off++;
        CHECK(off == 0, "m=%u: %u of %u half periods are not %u T (first %u)", mm, off,
              ne - 1, HALF(mm), ne > 1 ? edges[1] - edges[0] : 0);

        const double T = 6656.0 / 75.0;
        unsigned e = 0;
        int worst = 0;
        unsigned fractional = 0;
        for (size_t k = 0; k < ns; k++) {
            double a = T * (double)k, b = a + T, hi = 0, t = a;
            while (e < ne && (double)(uint32_t)(edges[e] - t0) < b) {
                double x = (double)(uint32_t)(edges[e] - t0);
                if (x > t) { if (lvl) hi += x - t; t = x; }
                lvl = !lvl;
                e++;
            }
            if (lvl) hi += b - t;
            int want = (int)lround(hi / T * BEEPER_FULL_SCALE);
            int d = abs(want - samples[k]);
            if (d > worst) worst = d;
            if (samples[k] != 0 && samples[k] != BEEPER_FULL_SCALE) fractional++;
        }
        CHECK(worst <= 1, "m=%u: samples should match the box filter to 1 LSB, worst %d",
              mm, worst);
        /* Not point sampling: samples that straddle an edge are fractional. */
        CHECK(fractional > ne / 2u, "m=%u: %u fractional samples for %u edges", mm,
              fractional, ne);
    }

    /* ---- the pitch, measured off the output (§15.2 M8) ----------------- *
     * The port's loop, draining per field, with the DC blocker in. */
    for (size_t p = 0; p < sizeof pitches / sizeof pitches[0]; p++) {
        unsigned mm = pitches[p];
        CHECK(start_beep(mm, 2000u), "m=%u: BEEP did not start", mm);
        ace_t *m = &g.m;
        restart_beeper(m, true);
        size_t ns = fields_draining(&g, 50, samples, MAX_SAMPLES);
        CHECK(m->beeper.overflow == 0, "m=%u: overflow %u", mm, m->beeper.overflow);

        /* Rising zero crossings, interpolated, after 0.1 s to settle. */
        const double T = 6656.0 / 75.0;
        const size_t settle = 3662;
        double first = -1, last = -1;
        unsigned crossings = 0;
        for (size_t k = settle; k < ns; k++) {
            if (samples[k - 1] < 0 && samples[k] >= 0) {
                double frac = (double)-samples[k - 1] / (double)(samples[k] - samples[k - 1]);
                double at = ((double)(k - 1) + frac) * T;
                if (first < 0) first = at;
                last = at;
                crossings++;
            }
        }
        double hz = (crossings - 1) * (double)ACE_CPU_HZ / (last - first);
        double want = (double)ACE_CPU_HZ / (2.0 * HALF(mm));
        printf("BEEP m=%u: %.3f Hz measured, %.3f Hz from the count (%u T a period)\n",
               mm, hz, want, 2u * HALF(mm));
        CHECK(fabs(hz - want) / want < 1e-4, "m=%u: measured %.3f Hz, expected %.3f Hz",
              mm, hz, want);

        /* The DC blocker centres it: a 50 % square wave swings about 0. */
        long sum = 0;
        int16_t lo = 0, hi = 0;
        for (size_t k = settle; k < ns; k++) {
            sum += samples[k];
            if (samples[k] < lo) lo = samples[k];
            if (samples[k] > hi) hi = samples[k];
        }
        double mean = (double)sum / (double)(ns - settle);
        CHECK(fabs(mean) < 200, "m=%u: mean %.1f should be near 0", mm, mean);
        CHECK(hi > 7000 && lo < -7000, "m=%u: swing %d..%d", mm, lo, hi);
    }

    /* ---- the note ends, and the prompt is silent again ---------------- */
    {
        CHECK(start_beep(100u, 100u), "BEEP did not start");
        ace_t *m = &g.m;
        restart_beeper(m, true);
        size_t ns = fields_draining(&g, 100, samples, MAX_SAMPLES);
        CHECK(m->cpu.iff1, "interrupts should be back on after the note");
        CHECK(abs(samples[ns - 1]) <= 1, "the speaker should come to rest at 0, got %d",
              samples[ns - 1]);
    }

    /* ---- a level held high decays to 0 ------------------------------- *
     * OUT ($FE),A; JR $-2 at $4000, with interrupts off: one step up. */
    {
        CHECK(guest_boot(&g, ACE_RAM_19K, 500), "no prompt");
        ace_t *m = &g.m;
        static const uint8_t held[] = { 0xD3, 0xFE, 0x18, 0xFE };
        memcpy(m->xram, held, sizeof held);
        m->cpu.pc = 0x4000;
        m->cpu.iff1 = m->cpu.iff2 = 0;
        restart_beeper(m, true);
        CHECK(!m->speaker, "the prompt leaves the speaker low");
        size_t ns = 0;
        for (unsigned f = 0; f < 60; f++) {
            ace_run_field(m);
            ns += ace_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        CHECK(m->speaker && m->beeper.edges > 0, "the OUT should raise the speaker");
        CHECK(samples[1] > 10000, "the step should be heard, got %d", samples[1]);
        CHECK(abs(samples[ns - 1]) <= 1, "a held level should decay to 0, got %d",
              samples[ns - 1]);
    }

    /* ---- nobody draining costs samples, and says so ------------------- */
    {
        CHECK(guest_boot(&g, ACE_RAM_19K, 500), "no prompt");
        ace_t *m = &g.m;
        restart_beeper(m, true);
        for (unsigned f = 0; f < 3; f++) ace_run_field(m);
        CHECK(m->beeper.count == ACE_AUDIO_BUF_LEN, "the buffer should be full");
        CHECK(m->beeper.overflow > 0, "the loss should be counted");

        /* A partial drain keeps the order. */
        int16_t a[10];
        m->beeper.buf[10] = 1234;
        CHECK(ace_audio_drain(m, a, 10) == 10, "partial drain");
        CHECK(m->beeper.buf[0] == 1234, "the rest should move up");
        CHECK(m->beeper.count == ACE_AUDIO_BUF_LEN - 10u, "count after drain");
    }

    /* ---- the rate follows the clock it is given ----------------------- */
    {
        ace_t *m = &g.m;
        ace_audio_set_rate(m, 125000000u, 4096u);
        CHECK(m->beeper.num == 13312u && m->beeper.den == 125u,
              "125 MHz / 4096 should be 13312/125 T a sample, got %u/%u",
              m->beeper.num, m->beeper.den);
    }

    /* ---- the T counter's wrap does not disturb the cadence ------------ */
    {
        CHECK(start_beep(100u, 2000u), "BEEP did not start");
        ace_t *m = &g.m;
        /* Move the clock to just short of the wrap, as ~22 minutes would. */
        uint32_t shift = 0xFFFFFFFFu - 100000u - m->cpu.t;
        m->cpu.t += shift;
        restart_beeper(m, true);
        uint32_t t0 = m->beeper.start;
        size_t total = fields_draining(&g, 20, samples, MAX_SAMPLES);
        uint64_t ran = (uint32_t)(m->cpu.t - t0);
        CHECK(m->cpu.t < t0, "the counter should have wrapped");
        CHECK(total == ran * 75u / 6656u, "across the wrap %llu T made %zu samples",
              (unsigned long long)ran, total);
        int quiet = 0;
        for (size_t k = 1000; k < total; k++) quiet += abs(samples[k]) < 1000;
        CHECK(quiet < (int)(total / 5u), "the note should sound across the wrap");
    }

    TEST_DONE();
}
