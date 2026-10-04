/* test_snapshot.c — our own save states, .sav (design.md §10.5; EL §8.5).
 *
 * The property that matters is executed, not compared: a machine saved
 * part-way through a program, restored into a machine that has been
 * doing something else and run on for 150 fields, must arrive exactly
 * where the original arrives, sound included. Then the refusals — a
 * damaged, short, foreign or newer file, another ROM, RAM size or field
 * — each leave the machine that refused it untouched.
 */

#include <stdio.h>
#include <string.h>

#include "ace_rom.h"
#include "guest.h"
#include "sha1.h"
#include "snapshot.h"
#include "test_util.h"

/* ---- a stream in memory --------------------------------------------- */

typedef struct {
    uint8_t buf[SNAP_HEADER_LEN + SNAP_STATE_LEN + 2u * ACE_BLOCK_BYTES +
                ACE_BLOCK_BYTES + ACE_XRAM_MAX + 16u];
    size_t  len, pos;
    size_t  fail_at;     /* fail the call that would cross this; 0 never */
} mem_t;

static bool mem_write(void *ctx, const uint8_t *src, size_t n) {
    mem_t *s = ctx;
    if (s->len + n > sizeof s->buf) return false;
    if (s->fail_at && s->len + n > s->fail_at) return false;
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    return true;
}

static bool mem_read(void *ctx, uint8_t *dst, size_t n) {
    mem_t *s = ctx;
    if (s->pos + n > s->len) return false;
    memcpy(dst, s->buf + s->pos, n);
    s->pos += n;
    return true;
}

static mem_t snap;

static snap_status_t check(const ace_t *m) { snap.pos = 0; return snapshot_check(m, mem_read, &snap); }
static snap_status_t load(ace_t *m)        { snap.pos = 0; return snapshot_load(m, mem_read, &snap); }
static snap_status_t save(const ace_t *m)  { snap.len = 0; return snapshot_save(m, mem_write, &snap); }

/* Everything a program can see, the CPU, and the machine's clock. */
static bool same(const ace_t *a, const ace_t *b, const char *what) {
    static const struct { const char *name; size_t off, len; } ram[] = {
        { "screen",  offsetof(ace_t, vram), ACE_BLOCK_BYTES },
        { "charset", offsetof(ace_t, cram), ACE_BLOCK_BYTES },
        { "user",    offsetof(ace_t, uram), ACE_BLOCK_BYTES },
        { "pack",    offsetof(ace_t, xram), ACE_XRAM_MAX },
    };
    for (unsigned r = 0; r < sizeof ram / sizeof ram[0]; r++) {
        const uint8_t *x = (const uint8_t *)a + ram[r].off, *y = (const uint8_t *)b + ram[r].off;
        for (size_t i = 0; i < ram[r].len; i++) {
            if (x[i] != y[i]) {
                fprintf(stderr, "    %s: %s +%zu %02X vs %02X\n", what, ram[r].name, i, x[i], y[i]);
                return false;
            }
        }
    }
    const z80_t *x = &a->cpu, *y = &b->cpu;
    if (x->af.w != y->af.w || x->bc.w != y->bc.w || x->de.w != y->de.w || x->hl.w != y->hl.w ||
        x->ix.w != y->ix.w || x->iy.w != y->iy.w || x->wz.w != y->wz.w || x->sp != y->sp ||
        x->pc != y->pc || x->af_ != y->af_ || x->bc_ != y->bc_ || x->de_ != y->de_ ||
        x->hl_ != y->hl_ || x->i != y->i || z80_r(x) != z80_r(y) || x->iff1 != y->iff1 ||
        x->iff2 != y->iff2 || x->im != y->im || x->q != y->q || x->halted != y->halted ||
        x->t != y->t) {
        fprintf(stderr, "    %s: CPU PC %04X/%04X SP %04X/%04X T %u/%u\n", what, x->pc, y->pc,
                x->sp, y->sp, (unsigned)x->t, (unsigned)y->t);
        return false;
    }
    if (a->speaker != b->speaker || a->budget != b->budget) {
        fprintf(stderr, "    %s: speaker %d/%d budget %d/%d\n", what, a->speaker, b->speaker,
                (int)a->budget, (int)b->budget);
        return false;
    }
    return true;
}

/* n fields, the sound drained after each into out (if not NULL). */
#define SOUND_MAX (200u * 800u)
static size_t run(guest_t *g, int n, int16_t *out) {
    size_t got = 0;
    int16_t scratch[ACE_AUDIO_BUF_LEN];
    for (int i = 0; i < n; i++) {
        keymatrix_field(&g->k, &g->m);
        ace_run_field(&g->m);
        size_t k = ace_audio_drain(&g->m, scratch, ACE_AUDIO_BUF_LEN);
        if (out && got + k <= SOUND_MAX) memcpy(out + got, scratch, k * sizeof scratch[0]);
        got += k;
    }
    return got;
}

static bool hex_digest_is(const void *msg, size_t len, const char *hex) {
    uint8_t d[SHA1_DIGEST_LEN];
    sha1(msg, len, d);
    char got[2 * SHA1_DIGEST_LEN + 1];
    for (unsigned i = 0; i < SHA1_DIGEST_LEN; i++) {
        static const char digits[] = "0123456789abcdef";
        got[2 * i] = digits[d[i] >> 4];
        got[2 * i + 1] = digits[d[i] & 15u];
    }
    got[2 * SHA1_DIGEST_LEN] = 0;
    return strcmp(got, hex) == 0;
}

static guest_t g, h;
static ace_t ahead, before;
static int16_t sound_a[SOUND_MAX], sound_b[SOUND_MAX];

/* A program that keeps the machine busy: the screen scrolling, the
 * speaker, and the stacks. */
static const char *const BUSY = ": spin 0 begin 1+ dup . 20 3 beep 0 until ; spin\n";

int main(void) {
    /* ---- the checks the format rests on --------------------------- */
    CHECK(snapshot_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "CRC-32 of 123456789 is %08X", (unsigned)snapshot_crc32(0, (const uint8_t *)"123456789", 9));
    /* pico-atom's SHA-1 cases, which came with sha1.c, and the ROM's
     * own hash, which the build checked with another implementation. */
    CHECK(hex_digest_is("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");
    CHECK(hex_digest_is("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"), "abc");
    {
        const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(hex_digest_is(two, strlen(two), "84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
              "the two-block message");
        uint8_t one[SHA1_DIGEST_LEN], parts[SHA1_DIGEST_LEN];
        sha1(ace_rom, ACE_ROM_SIZE, one);
        sha1_t s;
        sha1_init(&s);
        for (unsigned off = 0, step = 1; off < ACE_ROM_SIZE; off += step, step = step * 3u % 97u + 1u)
            sha1_update(&s, ace_rom + off, off + step > ACE_ROM_SIZE ? ACE_ROM_SIZE - off : step);
        sha1_final(&s, parts);
        CHECK(memcmp(one, parts, SHA1_DIGEST_LEN) == 0, "streamed digest differs");
    }
    CHECK(hex_digest_is(ace_rom, ACE_ROM_SIZE, "597ba8a15a292688333c84dc9fd35172abe5e7e6"),
          "the ROM's SHA-1 (design.md §10.2)");

    /* ---- save, run on, restore elsewhere, and meet ------------------ */
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    guest_type(&g, BUSY);
    uint32_t e0 = g.m.beeper.edges;
    run(&g, 37, NULL);
    CHECK(g.m.beeper.edges > e0, "the program should be making sound");

    CHECK(save(&g.m) == SNAP_OK, "save");
    CHECK(snap.len == snap_file_len(ACE_RAM_19K), "a 19K state is %zu bytes, want %u", snap.len,
          (unsigned)snap_file_len(ACE_RAM_19K));
    {
        /* RAM is written as it stands, and the ROM not at all. */
        const uint8_t *ram = snap.buf + SNAP_HEADER_LEN + SNAP_STATE_LEN;
        CHECK(memcmp(ram, g.m.vram, ACE_BLOCK_BYTES) == 0, "the screen first");
        CHECK(memcmp(ram + 3u * ACE_BLOCK_BYTES, g.m.xram, ACE_XRAM_19K) == 0, "then the pack");
        bool rom = false;
        for (size_t i = 0; i + 64u <= snap.len && !rom; i++)
            rom = memcmp(snap.buf + i, ace_rom + 0x1820u, 64u) == 0;
        CHECK(!rom, "ROM bytes must not be in a state");
        bool reserved = true;
        for (unsigned i = 80; i < SNAP_STATE_LEN; i++)
            reserved = reserved && snap.buf[SNAP_HEADER_LEN + i] == 0;
        CHECK(reserved, "reserved state bytes are written zero");
    }

    uint32_t ea = g.m.beeper.edges;
    size_t na = run(&g, 150, sound_a);
    ea = g.m.beeper.edges - ea;
    ace_copy(&ahead, &g.m);

    CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot the other");
    guest_type(&h, "2 2 + .\n");
    CHECK(!same(&ahead, &h.m, "control"), "control: a machine not restored must differ");
    CHECK(check(&h.m) == SNAP_OK, "check: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_OK, "load");
    keymatrix_init(&h.k);
    /* What the other machine had made and not yet drained is its own. */
    run(&h, 0, NULL);
    while (ace_audio_drain(&h.m, sound_b, ACE_AUDIO_BUF_LEN)) {}
    uint32_t eb = h.m.beeper.edges;
    size_t nb = run(&h, 150, sound_b);
    eb = h.m.beeper.edges - eb;
    CHECK(same(&ahead, &h.m, "resumed"), "a restored machine should run to the same state");
    /* The same edges at the same T, so the same sound; the sample grid
     * restarts at the restored clock (EL §8.5), so the samples can sit
     * up to one sample apart. */
    CHECK(ea > 0 && ea == eb, "the same speaker edges: %u and %u", (unsigned)ea, (unsigned)eb);
    CHECK(na + 1u >= nb && nb + 1u >= na, "and samples within one: %zu and %zu", na, nb);

    /* Control: the same restore with the budget one T out ends elsewhere,
     * so the comparison above would see a field that is not carried. */
    CHECK(load(&h.m) == SNAP_OK, "load again");
    h.m.budget += 1;
    keymatrix_init(&h.k);
    run(&h, 150, NULL);
    CHECK(!same(&ahead, &h.m, "control"), "control: a budget one T out must not meet");

    /* ---- refusals leave the machine as it was ----------------------- */
    CHECK(load(&h.m) == SNAP_OK, "load once more");
    ace_copy(&before, &h.m);

    snap.buf[SNAP_HEADER_LEN + SNAP_STATE_LEN + 0x950] ^= 0x01u;
    CHECK(check(&h.m) == SNAP_CORRUPT, "a flipped bit: %s", snapshot_status_str(check(&h.m)));
    snap.buf[SNAP_HEADER_LEN + SNAP_STATE_LEN + 0x950] ^= 0x01u;

    size_t full = snap.len;
    snap.len = full - 1000;
    CHECK(check(&h.m) == SNAP_IO, "truncated: %s", snapshot_status_str(check(&h.m)));
    snap.len = SNAP_HEADER_LEN + 10;
    CHECK(load(&h.m) == SNAP_IO, "torn in the state: %s", snapshot_status_str(load(&h.m)));
    snap.len = full;

    snap.buf[0] = 'X';
    CHECK(check(&h.m) == SNAP_NOT_SNAPSHOT, "magic: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_NOT_SNAPSHOT, "load refuses it too");
    snap.buf[0] = 'P';
    snap.buf[8] = SNAP_VERSION + 1;
    CHECK(check(&h.m) == SNAP_NEWER, "version: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_NEWER, "load refuses a newer version");
    snap.buf[8] = SNAP_VERSION;
    snap.buf[12] ^= 1u;
    CHECK(check(&h.m) == SNAP_NOT_SNAPSHOT, "a payload length no machine has: %s",
          snapshot_status_str(check(&h.m)));
    snap.buf[12] ^= 1u;
    CHECK(same(&before, &h.m, "untouched"), "refused files change nothing");

    /* Another ROM: one byte different, at a pointer the page table reads. */
    {
        static uint8_t rom2[ACE_ROM_SIZE];
        memcpy(rom2, ace_rom, sizeof rom2);
        rom2[0x1FFF] ^= 0xFFu;
        ace_config_t cfg;
        guest_config(&cfg, ACE_RAM_19K);
        cfg.rom = rom2;
        static ace_t other;
        CHECK(ace_init(&other, &cfg), "init with another ROM");
        CHECK(check(&other) == SNAP_OTHER_ROM, "another ROM: %s", snapshot_status_str(check(&other)));
        CHECK(load(&other) == SNAP_OTHER_ROM, "load refuses another ROM");

        /* Another field: INT a line longer. */
        guest_config(&cfg, ACE_RAM_19K);
        cfg.int_t += cfg.line_t;
        CHECK(ace_init(&other, &cfg), "init with another field");
        CHECK(check(&other) == SNAP_OTHER_FIELD, "another field: %s",
              snapshot_status_str(check(&other)));
        CHECK(load(&other) == SNAP_OTHER_FIELD, "load refuses another field");

        /* Other RAM sizes, which also round-trip their own states. */
        static const ace_ram_t sizes[] = { ACE_RAM_3K, ACE_RAM_51K };
        for (unsigned i = 0; i < 2; i++) {
            static guest_t k;
            CHECK(guest_boot(&k, sizes[i], 400), "boot %s", ace_ram_name(sizes[i]));
            static ace_t k_before;
            ace_copy(&k_before, &k.m);
            CHECK(load(&k.m) == SNAP_OTHER_RAM, "a 19K state in the %s: %s",
                  ace_ram_name(sizes[i]), snapshot_status_str(load(&k.m)));
            CHECK(same(&k_before, &k.m, "other RAM"), "and the %s is untouched",
                  ace_ram_name(sizes[i]));

            static mem_t own;
            own.len = own.pos = 0;
            guest_type(&k, ": sq dup * ;\n");
            CHECK(snapshot_save(&k.m, mem_write, &own) == SNAP_OK, "%s save", ace_ram_name(sizes[i]));
            CHECK(own.len == snap_file_len(sizes[i]), "%s file is %zu bytes", ace_ram_name(sizes[i]),
                  own.len);
            CHECK(guest_boot(&k, sizes[i], 400), "reboot %s", ace_ram_name(sizes[i]));
            CHECK(snapshot_check(&k.m, mem_read, &own) == SNAP_OK, "%s check", ace_ram_name(sizes[i]));
            own.pos = 0;
            CHECK(snapshot_load(&k.m, mem_read, &own) == SNAP_OK, "%s load", ace_ram_name(sizes[i]));
            keymatrix_init(&k.k);
            guest_type(&k, "7 sq .\n");
            if (!guest_screen_has(&k.m, "7 sq . 49  OK")) guest_dump(&k.m, stderr);
            CHECK(guest_screen_has(&k.m, "7 sq . 49  OK"), "%s: the restored word runs:\n%s",
                  ace_ram_name(sizes[i]), guest_row(&k.m, 22));
        }
    }

    /* A load that fails after it has changed the machine is followed by
     * a power-on (snapio.h): the machine comes up as ace_init's, at the
     * sample rate the port set and with the DC blocker as it was. */
    {
        static guest_t p;
        CHECK(guest_boot(&p, ACE_RAM_51K, 400), "boot 51K");
        ace_audio_set_rate(&p.m, 150000000u, 4000u);
        p.m.beeper.dc_block = false;
        uint32_t num = p.m.beeper.num, den = p.m.beeper.den;
        memset(p.m.xram, 0x5A, sizeof p.m.xram);
        ace_power_on(&p.m);
        CHECK(p.m.beeper.num == num && p.m.beeper.den == den && !p.m.beeper.dc_block,
              "the rate is kept: %u/%u, want %u/%u", p.m.beeper.num, p.m.beeper.den, num, den);
        CHECK(p.m.cfg.ram == ACE_RAM_51K && p.m.cpu.pc == 0 && p.m.xram[100] == 0,
              "a 51K machine at power-on, RAM zeroed");
        keymatrix_init(&p.k);
        guest_fields(&p, 100);
        guest_type(&p, "6 7 * .\n");
        CHECK(guest_screen_has(&p.m, "6 7 * . 42  OK"), "and it boots and runs");
    }

    /* A write that fails part-way reports it. */
    snap.fail_at = 5000;
    CHECK(save(&g.m) == SNAP_IO, "a failed write is reported");
    snap.fail_at = 0;

    /* Not while the CPU is stalled on a tape call. */
    g.m.tape.op = TAPE_LOAD;
    CHECK(save(&g.m) == SNAP_BUSY, "busy save");
    CHECK(load(&g.m) == SNAP_BUSY, "busy load");
    g.m.tape.op = TAPE_NONE;

    TEST_DONE();
}
