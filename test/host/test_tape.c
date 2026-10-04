/* test_tape.c — tape phase 1 against the ROM's own routines (design.md
 * §10.3, §13.3; EL §8.2, §11.3).
 *
 * The ROM's SAVE, untrapped, drives its signal onto D3 of its OUTs; this
 * records it. Played back into the tape input, the ROM's own LOAD and
 * VERIFY read it. Each time either routine is entered the machine is
 * copied, and the copy is served by the trap instead: from the .tap the
 * trapped SAVE made of the same blocks. At the caller's return address
 * the two must be the same machine in every byte of RAM and every
 * register but R, which counts instructions the trap does not run.
 */

#include <string.h>

#include "guest.h"
#include "tape.h"
#include "test_util.h"

/* ---- watching the ROM ------------------------------------------------- *
 * The CPU's trap hook (z80.h), used to observe: it never stalls. */

#define MAX_CALLS 8

typedef struct {
    uint16_t entry_pc;        /* the routine                           */
    uint16_t ret;             /* the caller's return address           */
    ace_t    entry, exit;
    bool     returned;
} call_t;

static call_t   s_calls[MAX_CALLS];
static unsigned s_n;
static int      s_open = -1;      /* the call not yet returned from     */
static uint8_t  s_watch_lo[256] = {
    [TAPE_SAVE_PC & 0xFFu] = 1,
    [TAPE_LOAD_PC & 0xFFu] = 1,
};

/* The signal, as (T since the first edge, level) pairs. */
#define MAX_EDGES 40000u
static uint32_t s_edge_t[MAX_EDGES];
static uint8_t  s_edge_l[MAX_EDGES];
static unsigned s_edges;
static bool     s_level;          /* D3 of the last OUT                 */
static bool     s_recording;
static uint32_t s_rec_t0;

/* Playback: idle until s_play is set, then the edges from s_play_t0. */
static bool     s_play;
static uint32_t s_play_t0;
static unsigned s_play_from;      /* the first edge played               */
/* The input idles high (D5 set, ace.c). Played inverted, the ROM's
 * signal rests at that level between and after blocks, as the silence
 * of a real tape must; played as recorded it rests low. */
static bool     s_invert = true;

static void (*s_io_write)(void *, uint16_t, uint8_t);
static uint8_t (*s_io_read)(void *, uint16_t);

static bool watch(void *ctx) {
    ace_t *m = ctx;
    uint16_t pc = m->cpu.pc;
    if (s_open >= 0 && pc == s_calls[s_open].ret) {
        ace_copy(&s_calls[s_open].exit, m);
        s_calls[s_open].returned = true;
        s_open = -1;
        s_watch_lo[pc & 0xFFu] = 0;
        s_watch_lo[TAPE_SAVE_PC & 0xFFu] = s_watch_lo[TAPE_LOAD_PC & 0xFFu] = 1;
        return false;
    }
    if ((pc == TAPE_SAVE_PC || pc == TAPE_LOAD_PC) && s_open < 0 && s_n < MAX_CALLS) {
        call_t *c = &s_calls[s_n];
        c->entry_pc = pc;
        c->ret = (uint16_t)(ace_peek(m, m->cpu.sp) | (ace_peek(m, (uint16_t)(m->cpu.sp + 1u)) << 8));
        c->returned = false;
        ace_copy(&c->entry, m);
        s_open = (int)s_n++;
        s_watch_lo[c->ret & 0xFFu] = 1;
        if (pc == TAPE_SAVE_PC && !s_recording) {
            s_recording = true;
            s_rec_t0 = m->cpu.t;
        }
        if (pc == TAPE_LOAD_PC && !s_play) {
            s_play = true;
            s_play_t0 = m->cpu.t + 10000u;
        }
    }
    return false;
}

static void rec_write(void *ctx, uint16_t port, uint8_t v) {
    ace_t *m = ctx;
    bool level = (v & 0x08u) != 0;   /* MAME's D3 (§16) */
    if (!(port & 1u) && s_recording && level != s_level && s_edges < MAX_EDGES) {
        s_edge_t[s_edges] = m->cpu.t - s_rec_t0;
        s_edge_l[s_edges] = level;
        s_edges++;
    }
    if (!(port & 1u)) s_level = level;
    s_io_write(ctx, port, v);
}

static uint8_t play_read(void *ctx, uint16_t port) {
    ace_t *m = ctx;
    bool idle = true;
    bool level = idle;
    if (s_play && s_edges > s_play_from) {
        uint32_t base = s_edge_t[s_play_from];
        uint32_t now = m->cpu.t - s_play_t0;
        /* The last edge at or before now; the line idles once the
         * signal is over. */
        unsigned lo = s_play_from, hi = s_edges;
        if (now >= 0x80000000u || now < s_edge_t[lo] - base) {
            level = idle;
        } else {
            while (hi - lo > 1u) {
                unsigned mid = (lo + hi) / 2u;
                if (s_edge_t[mid] - base <= now) lo = mid; else hi = mid;
            }
            bool d3 = s_edge_l[lo];
            level = s_invert ? !d3 : d3;
            if (lo == s_edges - 1u) level = idle;
        }
    }
    m->tape_in = level;
    return s_io_read(ctx, port);
}

static void hook(ace_t *m) {
    s_io_write = m->cpu.bus.io_write;
    s_io_read = m->cpu.bus.io_read;
    m->cpu.bus.io_write = rec_write;
    m->cpu.bus.io_read = play_read;
    m->cpu.bus.trap_lo = s_watch_lo;
    m->cpu.bus.trap = watch;
}

static void reset_watch(void) {
    s_n = 0;
    s_open = -1;
    memset(s_watch_lo, 0, sizeof s_watch_lo);
    s_watch_lo[TAPE_SAVE_PC & 0xFFu] = s_watch_lo[TAPE_LOAD_PC & 0xFFu] = 1;
    s_play = false;
}

static bool run_until_returned(guest_t *g, unsigned calls, int max_fields) {
    for (int f = 0; f < max_fields; f++) {
        if (s_n >= calls && s_calls[calls - 1u].returned) return true;
        guest_fields(g, 1);
    }
    return false;
}

/* ---- the .tap the trap writes ------------------------------------------ */

static uint8_t  s_tap[4096];
static size_t   s_tap_len;

/* Where each block's bytes start in s_tap, and how many there are,
 * checksum included. */
static size_t   s_blk_off[8], s_blk_len[8];
static unsigned s_blocks;

/* ---- comparing machines ------------------------------------------------- */

static int first_diff(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (int)i;
    return -1;
}

/* Everything but R, the T-state and instruction counts, the field's
 * position and the audio, none of which a trap can or should keep. */
static int same_machine(const ace_t *a, const ace_t *b, const char *what) {
    const z80_t *x = &a->cpu, *y = &b->cpu;
    int d;
    d = first_diff(a->vram, b->vram, sizeof a->vram);
    CHECK(d < 0, "%s: video RAM differs at $%04X: %02X, trap %02X", what,
          ACE_VRAM_BASE + d, d < 0 ? 0 : a->vram[d], d < 0 ? 0 : b->vram[d]);
    d = first_diff(a->cram, b->cram, sizeof a->cram);
    CHECK(d < 0, "%s: character RAM differs at +%d", what, d);
    d = first_diff(a->uram, b->uram, sizeof a->uram);
    CHECK(d < 0, "%s: user RAM differs at $%04X: %02X, trap %02X", what,
          ACE_URAM_BASE + d, d < 0 ? 0 : a->uram[d], d < 0 ? 0 : b->uram[d]);
    d = first_diff(a->xram, b->xram, sizeof a->xram);
    CHECK(d < 0, "%s: expansion RAM differs at $%04X: %02X, trap %02X", what,
          ACE_XRAM_BASE + d, d < 0 ? 0 : a->xram[d], d < 0 ? 0 : b->xram[d]);

#define REG(r) CHECK(x->r == y->r, "%s: " #r " is %04X, trap %04X", what, \
                     (unsigned)x->r, (unsigned)y->r)
    REG(af.w); REG(bc.w); REG(de.w); REG(hl.w);
    REG(ix.w); REG(iy.w); REG(sp); REG(pc); REG(wz.w);
    REG(af_); REG(bc_); REG(de_); REG(hl_);
    REG(i); REG(iff1); REG(iff2); REG(im); REG(q); REG(halted);
#undef REG
    CHECK(a->speaker == b->speaker, "%s: speaker %d, trap %d", what, a->speaker, b->speaker);
    return 0;
}

/* The trap on a copy of the machine at the routine's entry. A save's
 * block goes onto s_tap; a load is served block `blk` of s_tap. Then run
 * to the caller's return and compare. */
static int trapped(const call_t *c, int blk, const char *what) {
    static ace_t x;
    ace_copy(&x, &c->entry);
    /* The plain bus, no signal: the trap is the tape. */
    x.cpu.bus.io_read = s_io_read;
    x.cpu.bus.io_write = s_io_write;
    x.tape_in = true;
    x.cfg.tape_traps = true;
    tape_init(&x);
    CHECK(x.cpu.bus.trap != NULL, "%s: the trap stands aside for the stock ROM", what);

    uint32_t ran = ace_run(&x, 5000);
    const tape_t *t = ace_tape_pending(&x);
    CHECK(t != NULL && ran == 5000 && x.cpu.pc == c->entry_pc,
          "%s: stalled at the routine for the whole run", what);
    if (!t) return 1;
    CHECK(t->ret == c->ret, "%s: the return address is %04X, not %04X", what, t->ret, c->ret);

    if (t->op == TAPE_SAVE) {
        CHECK(s_blocks < 8 && s_tap_len + t->len + 3u <= sizeof s_tap, "%s: room", what);
        uint16_t n = (uint16_t)(t->len + 1u);
        s_tap[s_tap_len++] = (uint8_t)n;
        s_tap[s_tap_len++] = (uint8_t)(n >> 8);
        s_blk_off[s_blocks] = s_tap_len;
        s_blk_len[s_blocks] = n;
        s_blocks++;
        for (uint16_t i = 0; i < t->len; i++)
            s_tap[s_tap_len++] = ace_peek(&x, (uint16_t)(t->addr + i));
        s_tap[s_tap_len] = tape_checksum(s_tap + s_tap_len - t->len, t->len);
        s_tap_len++;
        CHECK(t->flag == tape_block_flag(s_blocks - 1u),
              "%s: the ROM writes a header, then data (flag %02X)", what, t->flag);
        ace_tape_save_end(&x);
    } else {
        bool want = ace_tape_load_begin(&x, tape_block_flag((uint32_t)blk));
        if (want) {
            /* In two pieces, to exercise the streaming. */
            size_t n = s_blk_len[blk], half = n / 2u;
            ace_tape_load_data(&x, s_tap + s_blk_off[blk], half);
            ace_tape_load_data(&x, s_tap + s_blk_off[blk] + half, n - half);
        }
        ace_tape_load_end(&x);
    }
    CHECK(ace_tape_pending(&x) == NULL, "%s: served", what);

    for (int i = 0; i < 200000 && x.cpu.pc != c->ret; i++) z80_step(&x.cpu);
    CHECK(x.cpu.pc == c->ret, "%s: the ROM returned to its caller", what);
    return same_machine(&c->exit, &x, what);
}

/* ---- the tests ------------------------------------------------------------ */

static guest_t g;

static int test_save(void) {
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    hook(&g.m);
    guest_type(&g, ": SQ DUP * ;\n");
    reset_watch();
    s_edges = 0;
    s_level = false;
    guest_type(&g, "SAVE SQ\n");
    CHECK(run_until_returned(&g, 2, 2000), "the ROM's SAVE wrote two blocks (%u calls)", s_n);
    s_recording = false;
    CHECK(s_edges > 9000u, "a signal was recorded: %u edges", s_edges);
    CHECK(s_calls[0].ret == TAPE_SAVE_HEADER_RET, "SAVE's header write returns to $%04X",
          s_calls[0].ret);
    if (test_failures) return 1;

    s_tap_len = 0;
    s_blocks = 0;
    if (trapped(&s_calls[0], -1, "SAVE header")) return 1;
    if (trapped(&s_calls[1], -1, "SAVE data")) return 1;
    CHECK(s_blocks == 2 && s_blk_len[0] == TAPE_HEADER_LEN + 1u,
          "a 25-byte header and its data: %u blocks, the first %zu bytes", s_blocks,
          s_blk_len[0]);
    CHECK(memcmp(s_tap + s_blk_off[0] + 1, "SQ        ", TAPE_NAME_LEN) == 0 ||
          memcmp(s_tap + s_blk_off[0] + 1, "sq        ", TAPE_NAME_LEN) == 0,
          "the header names SQ");
    return 0;
}

/* LOAD SQ into a machine that has never seen it, the ROM reading the
 * recorded signal from edge `from`; the calls it made are in s_calls. */
static int rom_load(const char *line, unsigned from, unsigned calls) {
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    hook(&g.m);
    reset_watch();
    s_play_from = from;
    guest_type(&g, line);
    CHECK(run_until_returned(&g, calls, 3000), "%s: the ROM made %u calls (%u)", line, calls,
          s_n);
    return test_failures ? 1 : 0;
}

static int test_load(void) {
    if (rom_load("LOAD SQ\n", 0, 2)) return 1;
    CHECK(s_calls[0].ret == TAPE_LOAD_HEADER_RET, "LOAD's header read returns to $%04X",
          s_calls[0].ret);
    /* The ROM's own load worked: the word is there. */
    guest_fields(&g, 50);
    guest_type(&g, "7 SQ .\n");
    CHECK(guest_screen_has(&g.m, "7 SQ . 49  OK"),
          "the ROM loaded SQ from its own signal");
    if (test_failures) { guest_dump(&g.m, stderr); return 1; }

    if (trapped(&s_calls[0], 0, "LOAD header")) return 1;
    if (trapped(&s_calls[1], 1, "LOAD data")) return 1;
    return 0;
}

static int test_verify(void) {
    /* VERIFY in the machine that loaded it. */
    reset_watch();
    s_play_from = 0;
    guest_type(&g, "VERIFY SQ\n");
    CHECK(run_until_returned(&g, 2, 3000), "VERIFY made two calls (%u)", s_n);
    if (test_failures) return 1;
    if (trapped(&s_calls[0], 0, "VERIFY header")) return 1;
    if (trapped(&s_calls[1], 1, "VERIFY data")) return 1;
    return 0;
}

/* A header asked for and data found: the flag byte differs, the ROM
 * returns with carry clear, and LOAD asks again. */
static int test_flag_mismatch(void) {
    /* The data block's leader starts at the first edge after the longest
     * gap in the signal. */
    unsigned from = 0;
    uint32_t gap = 0;
    for (unsigned i = 1; i < s_edges; i++) {
        if (s_edge_t[i] - s_edge_t[i - 1u] > gap) {
            gap = s_edge_t[i] - s_edge_t[i - 1u];
            from = i;
        }
    }
    if (rom_load("LOAD SQ\n", from, 1)) return 1;
    CHECK(!(s_calls[0].exit.cpu.af.b.l & Z80_FC), "the ROM's header read failed on data");
    if (trapped(&s_calls[0], 1, "LOAD header given data")) return 1;
    return 0;
}

/* The trap stands aside: a ROM whose routine differs, and a declined
 * request, run the ROM's own code. */
static int test_stands_aside(void) {
    static uint8_t rom[ACE_ROM_SIZE];
    static ace_t x;
    ace_config_t cfg;
    guest_config(&cfg, ACE_RAM_19K);
    CHECK(ace_init(&x, &cfg) && x.cpu.bus.trap != NULL, "the stock ROM is trapped");

    memcpy(rom, cfg.rom, sizeof rom);
    rom[TAPE_LOAD_PC + 1u] ^= 0xFFu;
    cfg.rom = rom;
    CHECK(ace_init(&x, &cfg) && x.cpu.bus.trap == NULL, "a changed routine is not");

    guest_config(&cfg, ACE_RAM_19K);
    cfg.tape_traps = false;
    CHECK(ace_init(&x, &cfg) && x.cpu.bus.trap == NULL, "traps off");

    /* Declined: the next run executes the routine's DI, and goes on in
     * the ROM's leader loop. */
    if (!s_n) return 1;
    ace_copy(&x, &s_calls[0].entry);
    x.cfg.tape_traps = true;
    tape_init(&x);
    ace_run(&x, 1000);
    CHECK(ace_tape_pending(&x) != NULL, "stalled");
    ace_tape_decline(&x);
    CHECK(ace_tape_pending(&x) == NULL, "declined");
    ace_run(&x, 100000);
    CHECK(ace_tape_pending(&x) == NULL && x.cpu.pc >= TAPE_LOAD_PC && x.cpu.pc < TAPE_ROM_END,
          "the ROM's routine runs, waiting for a signal (PC %04X)", x.cpu.pc);
    CHECK(x.cpu.iff1 == 0, "its DI ran");
    return 0;
}

/* The firmware's way: the guest runs field by field with the trap on,
 * and a request is served between fields from a deck over s_tap. */
static int deck_fields(guest_t *gg, int fields, unsigned *served) {
    static unsigned blk;
    for (int f = 0; f < fields; f++) {
        guest_fields(gg, 1);
        const tape_t *t = ace_tape_pending(&gg->m);
        if (!t) continue;
        if (t->op == TAPE_SAVE || blk >= s_blocks) {
            ace_tape_decline(&gg->m);
            continue;
        }
        if (ace_tape_load_begin(&gg->m, tape_block_flag(blk)))
            ace_tape_load_data(&gg->m, s_tap + s_blk_off[blk], s_blk_len[blk]);
        ace_tape_load_end(&gg->m);
        blk = (blk + 1u) % s_blocks;
        (*served)++;
    }
    return 0;
}

static int test_trapped_load(void) {
    static guest_t h;
    unsigned served = 0;
    CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot");
    CHECK(h.m.cpu.bus.trap != NULL, "the trap is on by default");
    guest_type(&h, "LOAD SQ\n");
    deck_fields(&h, 20, &served);
    guest_type(&h, "6 SQ .\n");
    CHECK(served == 2 && guest_screen_has(&h.m, "6 SQ . 36  OK"),
          "LOAD SQ through the trap alone: %u blocks served", served);

    /* A data byte damaged: the ROM's checksum test fails it. */
    s_tap[s_blk_off[1] + 2u] ^= 0x40u;
    guest_type(&h, "LOAD SQ\n");
    deck_fields(&h, 20, &served);
    CHECK(served == 4, "two more blocks served (%u)", served);
    bool err = false;
    for (int r = 0; r < (int)ACE_SCREEN_ROWS; r++)
        if (strstr(guest_row(&h.m, r), "ERROR")) err = true;
    CHECK(err, "a bad checksum is the ROM's error");
    s_tap[s_blk_off[1] + 2u] ^= 0x40u;
    if (test_failures) guest_dump(&h.m, stderr);
    return 0;
}

int main(void) {
    if (test_save()) TEST_DONE();
    if (test_load()) TEST_DONE();
    if (test_verify()) TEST_DONE();
    if (test_flag_mismatch()) TEST_DONE();
    test_stands_aside();
    test_trapped_load();
    TEST_DONE();
}
