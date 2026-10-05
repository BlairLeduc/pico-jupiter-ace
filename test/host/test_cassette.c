/* test_cassette.c — tape phase 2 against the ROM's own routines
 * (design.md §10.4, §13.3; EL §8.3, §11.3).
 *
 * The ROM's SAVE, untrapped, drives D3 of its OUTs. The core's recorder
 * decodes that into a .tap, and so does an independent decoder here,
 * from the edges this test records off the same OUTs; both must give
 * the .tap the trap writes for the same blocks. The player's half-cycles
 * must be the recorded ones edge for edge. Then the .tap goes back in
 * through the ROM's own LOAD and VERIFY with the trap off, and the word
 * runs.
 */

#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "tape.h"
#include "test_util.h"

/* ---- watching the port ------------------------------------------------- */

#define MAX_EDGES 60000u

typedef struct {
    uint32_t t[MAX_EDGES];
    unsigned n;
    bool     level;
} line_t;

static line_t   s_d3;            /* D3 of every even OUT             */
static line_t   s_access;        /* the speaker: IN low, OUT high     */
static bool     s_watching;
static uint32_t s_t0;

static void (*s_io_write)(void *, uint16_t, uint8_t);
static uint8_t (*s_io_read)(void *, uint16_t);

static void line_to(line_t *l, uint32_t t, bool level) {
    if (level == l->level) return;
    l->level = level;
    if (l->n < MAX_EDGES) l->t[l->n++] = t;
}

static void watch_write(void *ctx, uint16_t port, uint8_t v) {
    ace_t *m = ctx;
    if (s_watching && !(port & 1u)) {
        line_to(&s_d3, m->cpu.t - s_t0, (v & 0x08u) != 0);
        line_to(&s_access, m->cpu.t - s_t0, true);
    }
    s_io_write(ctx, port, v);
}

static uint8_t watch_read(void *ctx, uint16_t port) {
    ace_t *m = ctx;
    if (s_watching && !(port & 1u)) line_to(&s_access, m->cpu.t - s_t0, false);
    return s_io_read(ctx, port);
}

static void watch(ace_t *m) {
    s_io_write = m->cpu.bus.io_write;
    s_io_read = m->cpu.bus.io_read;
    m->cpu.bus.io_write = watch_write;
    m->cpu.bus.io_read = watch_read;
    memset(&s_d3, 0, sizeof s_d3);
    memset(&s_access, 0, sizeof s_access);
    s_d3.level = m->tape_out;
    s_access.level = m->speaker;
    s_t0 = m->cpu.t;
    s_watching = true;
}

/* ---- an independent decoder ---------------------------------------------- *
 * Not the recorder's state machine: it works on the whole list, splits
 * it at silences, finds the sync as the first short half after the
 * leader, and reads each bit's cycle as the load routine at $18FC does,
 * against the midpoint of a 0's and a 1's. */

static size_t decode(const line_t *l, uint8_t *out, size_t cap, unsigned *blocks) {
    size_t len = 0;
    *blocks = 0;
    unsigned i = 0;
    while (i + 1u < l->n) {
        /* A block's edges run until a half longer than a leader's. */
        unsigned end = i + 1u;
        while (end < l->n && l->t[end] - l->t[end - 1u] < 3000u) end++;
        /* Leader: halves of about 2,011 T; the sync is the first short one. */
        unsigned k = i + 1u, lead = 0;
        while (k < end && l->t[k] - l->t[k - 1u] > 1500u) { k++; lead++; }
        if (lead < 100u || k + 1u >= end) { i = end; continue; }
        k += 2u;                    /* the two sync edges: k is the first bit's */
        uint8_t bytes[65536];
        size_t nb = 0;
        unsigned bit = 0;
        uint8_t b = 0;
        while (k + 1u < end) {
            uint32_t cycle = l->t[k + 1u] - l->t[k - 1u];
            b = (uint8_t)((b << 1) | (cycle > 2393u));
            k += 2u;
            if (++bit == 8u) { if (nb < sizeof bytes) bytes[nb++] = b; bit = 0; b = 0; }
        }
        if (nb >= 2u && len + 1u + nb <= cap) {
            uint32_t n = (uint32_t)(nb - 1u);
            out[len++] = (uint8_t)n;
            out[len++] = (uint8_t)(n >> 8);
            memcpy(out + len, bytes + 1, n);      /* the flag is the place */
            len += n;
            (*blocks)++;
        }
        i = end;
    }
    return len;
}

/* ---- the trap's .tap, for the same SAVE ------------------------------------ */

static size_t trapped_save(guest_t *g, uint8_t *out, size_t cap) {
    size_t len = 0;
    for (int f = 0; f < 400; f++) {
        guest_fields(g, 1);
        const tape_t *t = ace_tape_pending(&g->m);
        if (!t) continue;
        CHECK(t->op == TAPE_SAVE, "the trap saw a save");
        uint32_t n = t->len + 1u;
        if (len + 2u + n > cap) return 0;
        out[len++] = (uint8_t)n;
        out[len++] = (uint8_t)(n >> 8);
        uint8_t x = 0;
        for (uint32_t i = 0; i < t->len; i++) {
            uint8_t b = ace_peek(&g->m, (uint16_t)(t->addr + i));
            x ^= b;
            out[len++] = b;
        }
        out[len++] = x;
        bool data = t->flag == TAPE_FLAG_DATA;
        ace_tape_save_end(&g->m);
        if (data) break;
    }
    return len;
}

/* ---- the tests ---------------------------------------------------------------- */

static guest_t g, h;
static uint8_t s_tape[65536];      /* the recorder's image             */
static size_t  s_tape_len;

static void traps_off(ace_t *m) {
    m->cfg.tape_traps = false;
    tape_hook(m);
}

static bool run_until(guest_t *gg, bool (*done)(const ace_t *), int max_fields) {
    for (int f = 0; f < max_fields; f++) {
        if (done(&gg->m)) return true;
        guest_fields(gg, 1);
    }
    return done(&gg->m);
}

static bool saved_two(const ace_t *m) { return m->cas.rec.blocks >= 2u && !m->cas.rec.on; }

static int test_record(void) {
    /* The trap's .tap of the same word, in a machine of its own. */
    static uint8_t want[65536];
    CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot");
    guest_type(&h, ": SQ DUP * ;\n");
    guest_type(&h, "SAVE SQ\n");
    size_t want_len = trapped_save(&h, want, sizeof want);
    CHECK(want_len == 2u + 26u + 2u + want[28] + (want[29] << 8), "the trap saved two blocks");

    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, s_tape, 0, sizeof s_tape);
    ace_cassette_record(&g.m, true);
    guest_type(&g, ": SQ DUP * ;\n");
    watch(&g.m);
    guest_type(&g, "SAVE SQ\n");
    CHECK(run_until(&g, saved_two, 2000), "the recorder took two blocks (%u)",
          g.m.cas.rec.blocks);
    s_watching = false;
    s_tape_len = g.m.cas.len;
    CHECK(g.m.cas.rec.errors == 0 && !g.m.cas.rec.full, "no block dropped");
    CHECK(g.m.tape.served == 0 && g.m.tape.op == TAPE_NONE, "the trap never stalled");

    CHECK(s_tape_len == want_len && memcmp(s_tape, want, want_len) == 0,
          "the recorder's .tap is the trap's: %zu bytes, %zu", s_tape_len, want_len);

    static uint8_t mine[65536];
    unsigned blocks;
    size_t mine_len = decode(&s_d3, mine, sizeof mine, &blocks);
    CHECK(blocks == 2 && mine_len == want_len && memcmp(mine, want, want_len) == 0,
          "D3 decodes independently to the same .tap: %u blocks, %zu bytes", blocks, mine_len);

    /* The speaker's line, IN low and OUT high, carries no tape: SAVE's
     * signal is on D3 alone (§16). */
    size_t spk_len = decode(&s_access, mine, sizeof mine, &blocks);
    CHECK(blocks == 0 && spk_len == 0, "the access line decodes to nothing (%u blocks)",
          blocks);
    printf("SAVE SQ: %u D3 edges, %u speaker edges, %zu-byte .tap\n", s_d3.n, s_access.n,
           s_tape_len);
    /* For a check in another emulator (tools/trace, design.md §15.2 M13). */
    const char *out = getenv("PICO_ACE_TAP_OUT");
    if (out) {
        FILE *f = fopen(out, "wb");
        CHECK(f && fwrite(s_tape, 1, s_tape_len, f) == s_tape_len, "write %s", out);
        if (f) fclose(f);
    }
    return test_failures ? 1 : 0;
}

/* The player's walk against the ROM's edges, half-cycle for half-cycle:
 * from each block's first edge to its last, which is the exit's. */
static int test_timing(void) {
    cassette_t c;
    memset(&c, 0, sizeof c);
    c.img = s_tape;
    c.len = c.cap = (uint32_t)s_tape_len;
    c.loaded = true;

    unsigned e = 0, block = 0, mismatches = 0;
    uint32_t t;
    bool toggles, first = true;
    uint32_t gap_rom = 0, gap_player = 0;
    while (cassette_walk(&c, first, &t, &toggles)) {
        if (!toggles) {
            /* The gap: where the ROM's next block starts. */
            if (e < s_d3.n) gap_rom = s_d3.t[e] - s_d3.t[e - 1u];
            block++;
            /* The next block's lead-in: its first edge is the start. */
            uint32_t gap = t;
            if (!cassette_walk(&c, false, &t, &toggles)) break;
            CHECK(toggles, "a leader follows the gap");
            gap_player = gap + t;
            e++;
            continue;
        }
        if (first) {                          /* the lead-in to the first edge */
            first = false;
            e = 1;
            continue;
        }
        if (e >= s_d3.n) { mismatches++; break; }
        uint32_t rom = s_d3.t[e] - s_d3.t[e - 1u];
        if (rom != t && mismatches++ < 5u)
            printf("block %u, edge %u: the ROM's half is %u T, the player's %u\n", block, e,
                   rom, t);
        e++;
    }
    CHECK(mismatches == 0, "every half-cycle the ROM's: %u differ", mismatches);
    CHECK(e == s_d3.n, "as many edges: %u played, %u recorded", e, s_d3.n);
    CHECK(block == 2, "two blocks walked (%u)", block);
    CHECK(gap_rom == gap_player, "the gap between header and data: the ROM's %u T, the "
          "player's %u", gap_rom, gap_player);
    return test_failures ? 1 : 0;
}

/* LOAD and VERIFY through the ROM's own routine, the trap off. */
static int test_load(void) {
    static uint8_t img[65536];
    memcpy(img, s_tape, s_tape_len);
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, img, (uint32_t)s_tape_len, (uint32_t)s_tape_len);
    CHECK(g.m.cpu.bus.trap != NULL && g.m.cpu.bus.trap_lo != NULL,
          "a tape in the deck hooks the cues");
    uint32_t t0 = g.m.cpu.t;
    guest_type(&g, "LOAD SQ\n");
    bool played = false;
    uint32_t t_play = 0;
    /* Until the deck has stopped in the data block: the header's exit
     * stops it in the header's last edge. */
    for (int f = 0; f < 3000 && !(played && !g.m.cas.playing && g.m.cas.index >= 1u); f++) {
        uint32_t before = g.m.cpu.t;
        bool was = g.m.cas.playing;
        guest_fields(&g, 1);
        if (was || g.m.cas.playing) { played = true; t_play += g.m.cpu.t - before; }
    }
    guest_fields(&g, 50);
    CHECK(played && !g.m.cas.playing, "the load's cue started the deck and its exit stopped it");
    CHECK(g.m.tape.served == 0, "the trap served nothing");
    guest_type(&g, "7 SQ .\n");
    CHECK(guest_screen_has(&g.m, "7 SQ . 49  OK"), "the ROM loaded SQ off the signal");
    if (test_failures) { guest_dump(&g.m, stderr); return 1; }
    printf("LOAD SQ: %u edges played in %.2f s of guest time (%u T since typing)\n",
           g.m.cas.edges, t_play / 3250000.0, g.m.cpu.t - t0);

    ace_cassette_rewind(&g.m);
    guest_type(&g, "VERIFY SQ\n");
    guest_fields(&g, 600);
    CHECK(!g.m.cas.playing, "VERIFY stopped the deck");
    bool err = false;
    for (int r = 0; r < (int)ACE_SCREEN_ROWS; r++)
        if (strstr(guest_row(&g.m, r), "ERROR")) err = true;
    CHECK(!err, "VERIFY SQ agrees");
    if (test_failures) guest_dump(&g.m, stderr);
    return test_failures ? 1 : 0;
}

/* A data byte damaged on the tape: the ROM's checksum test fails it. */
static int test_damaged(void) {
    static uint8_t img[65536];
    memcpy(img, s_tape, s_tape_len);
    img[28 + 2 + 3] ^= 0x10u;
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, img, (uint32_t)s_tape_len, (uint32_t)s_tape_len);
    guest_type(&g, "LOAD SQ\n");
    guest_fields(&g, 600);
    bool err = false;
    for (int r = 0; r < (int)ACE_SCREEN_ROWS; r++)
        if (strstr(guest_row(&g.m, r), "ERROR")) err = true;
    CHECK(err, "a bad byte is the ROM's error");
    if (test_failures) guest_dump(&g.m, stderr);
    return test_failures ? 1 : 0;
}

/* The firmware's way: the trap on, every request declined, and the deck
 * following the ROM's cues; a save recorded and handed out unsaved. */
static int test_declined(void) {
    static uint8_t img[65536];
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    ace_cassette_insert(&g.m, img, 0, sizeof img);
    ace_cassette_record(&g.m, true);
    guest_type(&g, ": CU DUP DUP * * ;\n");
    guest_type(&g, "SAVE CU\n");
    unsigned declined = 0;
    for (int f = 0; f < 2000 && g.m.cas.rec.blocks < 2u; f++) {
        guest_fields(&g, 1);
        if (ace_tape_pending(&g.m)) { ace_tape_decline(&g.m); declined++; }
    }
    guest_fields(&g, 5);
    uint32_t from = 0, to = 0;
    CHECK(declined == 2 && g.m.cas.rec.blocks == 2 && ace_cassette_unsaved(&g.m, &from, &to) &&
          from == 0 && to == g.m.cas.len,
          "two declined saves recorded, unsaved [%u, %u)", from, to);
    ace_cassette_saved(&g.m, true);
    CHECK(!ace_cassette_unsaved(&g.m, &from, &to), "saved");
    ace_cassette_record(&g.m, false);
    uint32_t len = g.m.cas.len;

    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    ace_cassette_insert(&g.m, img, len, len);
    guest_type(&g, "LOAD CU\n");
    declined = 0;
    for (int f = 0; f < 3000; f++) {
        guest_fields(&g, 1);
        if (ace_tape_pending(&g.m)) { ace_tape_decline(&g.m); declined++; }
        if (declined >= 2 && !g.m.cas.playing) break;
    }
    guest_fields(&g, 50);
    guest_type(&g, "3 CU .\n");
    CHECK(declined == 2 && guest_screen_has(&g.m, "3 CU . 27  OK"),
          "LOAD CU through declines and cues (%u declined)", declined);
    if (test_failures) guest_dump(&g.m, stderr);
    return test_failures ? 1 : 0;
}

/* A block is kept only whole. With room for the header but not the
 * data, the data is taken back out; a reset part-way through a block
 * leaves the image as it was before the block. */
static int test_cut_short(void) {
    static uint8_t img[65536];
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, img, 0, 28u + 10u);
    ace_cassette_record(&g.m, true);
    guest_type(&g, ": SQ DUP * ;\n");
    guest_type(&g, "SAVE SQ\n");
    guest_fields(&g, 600);
    CHECK(g.m.cas.len == 28u && g.m.cas.rec.blocks == 1u && g.m.cas.rec.errors == 1u,
          "no room for the data: the header alone kept (%u bytes, %u blocks, %u dropped)",
          g.m.cas.len, g.m.cas.rec.blocks, g.m.cas.rec.errors);

    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, img, 0, sizeof img);
    ace_cassette_record(&g.m, true);
    guest_type(&g, ": SQ DUP * ;\n");
    guest_type(&g, "SAVE SQ\n");
    /* In small slices, through the CPU's trap hook as a field would. */
    for (long k = 0; k < 2000000 && !(g.m.cas.rec.blocks == 1u && g.m.cas.rec.open &&
                                      g.m.cas.rec.n >= 4u); k++)
        ace_run(&g.m, 100);
    CHECK(g.m.cas.rec.open && g.m.cas.rec.n >= 4u, "the data block part-way (%u bytes)",
          g.m.cas.rec.n);
    ace_reset(&g.m);
    CHECK(g.m.cas.len == 28u && !g.m.cas.rec.on && g.m.cas.rec.errors == 1u,
          "a reset part-way: the image as it was (%u bytes)", g.m.cas.len);
    return test_failures ? 1 : 0;
}

/* Played by hand, with no load running: the key scan's INs read the
 * deck along, a stop keeps the place, and the tape plays to its end with
 * every edge of the walk. */
static int test_by_hand(void) {
    static uint8_t img[65536];
    memcpy(img, s_tape, s_tape_len);
    cassette_t c;
    memset(&c, 0, sizeof c);
    c.img = img;
    c.len = c.cap = (uint32_t)s_tape_len;
    uint32_t t, total = 0;
    bool toggles, first = true;
    while (cassette_walk(&c, first, &t, &toggles)) { first = false; total += toggles; }

    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    ace_cassette_insert(&g.m, img, (uint32_t)s_tape_len, (uint32_t)s_tape_len);
    ace_cassette_play(&g.m, true);
    guest_fields(&g, 20);
    ace_cassette_play(&g.m, false);       /* brings it up to now first */
    uint32_t e1 = g.m.cas.edges;
    guest_fields(&g, 20);
    CHECK(e1 > 0 && g.m.cas.edges == e1 && g.m.tape_in, "stopped where it was, the input idle: "
          "%u edges, then %u, input %d", e1, g.m.cas.edges, g.m.tape_in);
    ace_cassette_play(&g.m, true);
    guest_fields(&g, 1000);
    CHECK(g.m.cas.ended && !g.m.cas.playing && g.m.cas.edges == total,
          "played to the end: %u edges of %u", g.m.cas.edges, total);
    ace_cassette_play(&g.m, true);
    CHECK(!g.m.cas.playing, "at the end it does not start");
    ace_cassette_rewind(&g.m);
    ace_cassette_play(&g.m, true);
    CHECK(g.m.cas.playing && !g.m.cas.ended, "rewound, it plays");
    return test_failures ? 1 : 0;
}

/* An archive .tap, when PICO_ACE_TAP names one: LOAD by the first
 * header's name off the signal, then the word run for 200 fields. */
static int test_archive(void) {
    const char *path = getenv("PICO_ACE_TAP");
    if (!path) return 0;
    static uint8_t img[65536];
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "open %s", path);
    if (!f) return 1;
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    char name[TAPE_NAME_LEN + 1];
    memcpy(name, img + 3, TAPE_NAME_LEN);
    name[TAPE_NAME_LEN] = 0;
    for (int i = TAPE_NAME_LEN - 1; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    traps_off(&g.m);
    ace_cassette_insert(&g.m, img, (uint32_t)n, (uint32_t)n);
    char line[40];
    snprintf(line, sizeof line, "%s %s\n", img[2] ? "0 0 BLOAD" : "LOAD", name);
    guest_type(&g, line);
    /* Until the deck stops in the data block, timing the play. */
    uint32_t played = 0;
    for (int k = 0; k < 20000 && !(g.m.cas.index >= 1u && !g.m.cas.playing && played); k++) {
        uint32_t before = g.m.cpu.t;
        bool was = g.m.cas.playing;
        guest_fields(&g, 1);
        if (was || g.m.cas.playing) played += g.m.cpu.t - before;
    }
    printf("archive %s off the signal: %.1f s of guest time playing\n", name,
           played / 3250000.0);
    guest_fields(&g, 50);
    guest_dump(&g.m, stdout);
    return 0;
}

int main(void) {
    if (test_record()) TEST_DONE();
    if (test_timing()) TEST_DONE();
    test_load();
    test_damaged();
    test_declined();
    test_by_hand();
    test_cut_short();
    test_archive();
    TEST_DONE();
}
