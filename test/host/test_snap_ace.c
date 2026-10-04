/* test_snap_ace.c — .ace import (design.md §10.5, §15.2 M11).
 *
 * The files are made here, from machines the real ROM has run, by an
 * encoder written to the archive's description of ACE32's format (the
 * Jupiter Ace Archive's FAQ, fetched 2026-10-04), with noise in the high
 * halves of the register words as the archive's files have. A file is
 * judged by executing it: once loaded, a word defined before the save
 * must run when typed. The refusals each leave the machine untouched,
 * and the key wait's repair has a control that shows the word it writes
 * is the one the ROM needs.
 *
 * With PICO_ACE_ACE_DIR set, every .ace in that directory is checked
 * and, where it loads, run (the archive's files are not committed). With
 * PICO_ACE_ACE_FILE and PICO_ACE_ACE_DUMP set, that one file is loaded
 * into the machine it needs, run ACE_DUMP_FIELDS fields (default 100),
 * and dumped in tools/mame/ace-dump.lua's form, for
 * tools/ace-reference.sh to compare with MAME.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "snap_ace.h"
#include "test_util.h"

/* ---- a stream in memory ------------------------------------------------ */

typedef struct {
    uint8_t buf[3u * 65536u];
    size_t  len, pos;
    size_t  fail_at;      /* a read error at this offset; 0 never      */
    size_t  piece;        /* most bytes a read returns; 0 for 97       */
} mem_t;

static int mem_read(void *ctx, uint8_t *dst, size_t max) {
    mem_t *s = ctx;
    size_t n = s->piece ? s->piece : 97u;       /* odd, to cross runs */
    if (n > max) n = max;
    if (s->fail_at && s->pos + n > s->fail_at) return -1;
    if (n > s->len - s->pos) n = s->len - s->pos;
    memcpy(dst, s->buf + s->pos, n);
    s->pos += n;
    return (int)n;
}

static mem_t file;

static snap_ace_status_t check(const ace_t *m, snap_ace_info_t *in) {
    file.pos = 0;
    return snap_ace_check(m, mem_read, &file, in);
}
static snap_ace_status_t load(ace_t *m, snap_ace_info_t *in) {
    file.pos = 0;
    return snap_ace_load(m, mem_read, &file, in);
}

/* ---- ACE32's format, written ------------------------------------------- */

static uint8_t image[0x10000];

static void put32(uint32_t addr, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) image[addr + i] = (uint8_t)(v >> (8u * i));
}

/* A register word: the value, and noise above it, from bit 16 for a
 * pair and from bit 8 for the bytes (IM on), as the archive's have. */
static void reg(unsigned n, uint16_t v) {
    uint32_t noise = n * 0x9E3779B9u + 0x12345678u;
    put32(0x2100u + 4u * n, n < 12u ? (v | (noise & 0xFFFF0000u)) : (v | (noise & 0xFFFFFF00u)));
}

/* The machine's address space from $2000 to `end` as ACE32 lays it out:
 * its state in the screen's mirror, the mirrors otherwise zero. */
static void lay_out(const ace_t *m, uint32_t ramtop, uint32_t end) {
    memset(image, 0, sizeof image);
    for (uint32_t a = 0x2400u; a < end && a < 0x10000u; a++) {
        if ((a >= 0x2800u && a < 0x2C00u) || (a >= 0x3000u && a < 0x3C00u)) continue;
        if (a >= 0x2C00u && a < 0x3000u) image[a] = m->cram[a - 0x2C00u];
        else image[a] = ace_peek(m, (uint16_t)a);
    }
    put32(0x2000u, 0x8001u);
    put32(0x2080u, ramtop & 0xFFFFu);
    const z80_t *c = &m->cpu;
    const uint16_t r[17] = { c->af.w, c->bc.w, c->de.w, c->hl.w, c->ix.w, c->iy.w, c->sp,
                             c->pc, c->af_, c->bc_, c->de_, c->hl_, c->im, c->iff1, c->iff2,
                             c->i, z80_r(c) };
    for (unsigned i = 0; i < 17; i++) reg(i, r[i]);
}

/* Run-length encoded as ACE32 does: three or more alike, and every ED,
 * as ED n b with n at most 240; then ED 00. */
static void encode(uint32_t end) {
    file.len = 0;
    for (uint32_t a = 0x2000u; a < end;) {
        uint8_t v = image[a & 0xFFFFu];
        uint32_t n = 1;
        while (a + n < end && n < 240u && image[(a + n) & 0xFFFFu] == v) n++;
        if (n >= 3u || v == 0xEDu) {
            file.buf[file.len++] = 0xEDu;
            file.buf[file.len++] = (uint8_t)n;
            file.buf[file.len++] = v;
        } else {
            for (uint32_t i = 0; i < n; i++) file.buf[file.len++] = v;
        }
        a += n;
    }
    file.buf[file.len++] = 0xEDu;
    file.buf[file.len++] = 0x00u;
}

static void make(const ace_t *m, uint32_t ramtop, uint32_t end) {
    lay_out(m, ramtop, end);
    encode(end);
}

/* ---- the machines -------------------------------------------------------- */

static guest_t g, h;
static ace_t before;

static bool untouched(const ace_t *a, const ace_t *b) {
    return memcmp(a->vram, b->vram, sizeof a->vram) == 0 &&
           memcmp(a->cram, b->cram, sizeof a->cram) == 0 &&
           memcmp(a->uram, b->uram, sizeof a->uram) == 0 &&
           memcmp(a->xram, b->xram, sizeof a->xram) == 0 &&
           a->cpu.pc == b->cpu.pc && a->cpu.sp == b->cpu.sp && a->cpu.af.w == b->cpu.af.w;
}

/* A machine of `ram` at the prompt that knows SQ, with an ED byte in its
 * RAM so the encoder writes ED 01 ED. */
static bool with_sq(guest_t *x, ace_ram_t ram) {
    if (!guest_boot(x, ram, 400)) return false;
    guest_type(x, ": sq dup * ;\n");
    ace_poke(&x->m, 0x3FF0u, 0xEDu);
    return true;
}

/* Type "7 sq ." and see 49. */
static bool sq_runs(guest_t *x) {
    keymatrix_init(&x->k);
    guest_type(x, "7 sq .\n");
    return guest_screen_has(&x->m, "7 sq . 49  OK");
}

/* ---- the archive, if given ----------------------------------------------- */

static bool read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    file.len = fread(file.buf, 1, sizeof file.buf, f);
    file.pos = 0;
    fclose(f);
    return true;
}

static void dump(const ace_t *m, FILE *f) {
    const z80_t *c = &m->cpu;
    fprintf(f, "AF %04X\nBC %04X\nDE %04X\nHL %04X\nIX %04X\nIY %04X\nSP %04X\nPC %04X\n",
            c->af.w, c->bc.w, c->de.w, c->hl.w, c->ix.w, c->iy.w, c->sp, c->pc);
    fprintf(f, "AF2 %04X\nBC2 %04X\nDE2 %04X\nHL2 %04X\nI %04X\nIM %04X\nIFF1 %04X\nIFF2 %04X\n",
            c->af_, c->bc_, c->de_, c->hl_, c->i, c->im, c->iff1, c->iff2);
    static const uint32_t spans[2][2] = { { 0x2400u, 0x2800u }, { 0x3C00u, 0x8000u } };
    for (unsigned s = 0; s < 2; s++) {
        for (uint32_t a = spans[s][0]; a < spans[s][1]; a += 32u) {
            fprintf(f, "%04X ", (unsigned)a);
            for (unsigned i = 0; i < 32u; i++) fprintf(f, "%02X", ace_peek(m, (uint16_t)(a + i)));
            fputc('\n', f);
        }
    }
}

/* One file into the machine it needs, run, and dumped. */
static int dump_one(const char *path, const char *out) {
    if (!read_file(path)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    snap_ace_info_t in;
    if (!guest_boot(&g, ACE_RAM_19K, 400)) return 1;
    snap_ace_status_t st = check(&g.m, &in);
    if (st == SNAP_ACE_OTHER_RAM && !guest_boot(&g, in.needs, 400)) return 1;
    st = load(&g.m, &in);
    if (st != SNAP_ACE_OK) {
        fprintf(stderr, "%s: %s\n", path, snap_ace_status_str(st));
        return 2;
    }
    const char *fe = getenv("ACE_DUMP_FIELDS");
    keymatrix_init(&g.k);
    guest_fields(&g, fe ? atoi(fe) : 100);
    FILE *f = fopen(out, "w");
    if (!f) return 1;
    dump(&g.m, f);
    fclose(f);
    return 0;
}

static int archive(const char *dir) {
    DIR *d = opendir(dir);
    CHECK(d != NULL, "cannot open %s", dir);
    if (!d) return 1;
    unsigned n = 0, loaded = 0, repaired = 0, refused[SNAP_ACE_BUSY + 1] = { 0 };
    unsigned taken[4] = { 0 };
    struct dirent *e;
    static char path[1024];
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".ace") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!read_file(path)) continue;
        n++;
        snap_ace_info_t in;
        CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
        snap_ace_status_t st = check(&g.m, &in);
        if (st == SNAP_ACE_OTHER_RAM) {
            CHECK(guest_boot(&g, in.needs, 400), "boot %s", ace_ram_name(in.needs));
            st = check(&g.m, &in);
        }
        if (st != SNAP_ACE_OK) {
            refused[st]++;
            printf("  refused %-60.60s %s (taken on %s, SP %04X)\n", e->d_name,
                   snap_ace_status_str(st), snap_ace_taken_on(&in), in.sp);
            continue;
        }
        taken[in.ramtop == 0x4000u ? 0 : in.ramtop == 0x8000u ? 1 : in.ramtop == 0xC000u ? 2 : 3]++;
        CHECK(load(&g.m, &in) == SNAP_ACE_OK, "%s: load after check", e->d_name);
        loaded++;
        repaired += in.repaired;

        /* Loaded in the ROM's key wait, it is still there 100 fields on,
         * with the screen the file has. */
        bool waiting = in.pc == SNAP_ACE_WAIT_PC1 || in.pc == SNAP_ACE_WAIT_PC2;
        static uint8_t scr[ACE_SCREEN_BYTES];
        memcpy(scr, g.m.vram, sizeof scr);
        keymatrix_init(&g.k);
        guest_fields(&g, 100);
        if (waiting) {
            CHECK(g.m.cpu.pc == SNAP_ACE_WAIT_PC1 || g.m.cpu.pc == SNAP_ACE_WAIT_PC2,
                  "%s: left the key wait, PC %04X", e->d_name, g.m.cpu.pc);
            CHECK(memcmp(scr, g.m.vram, sizeof scr) == 0, "%s: the screen changed", e->d_name);
        }
    }
    closedir(d);
    printf("  %u files: %u loaded (taken on 3K %u, 19K %u, 35K %u, 51K %u; %u repaired), "
           "%u not .ace, %u without their stack\n",
           n, loaded, taken[0], taken[1], taken[2], taken[3], repaired,
           refused[SNAP_ACE_NOT_ACE], refused[SNAP_ACE_NO_STACK]);
    CHECK(n > 0, "no .ace files in %s", dir);
    TEST_DONE();
}

int main(void) {
    const char *one = getenv("PICO_ACE_ACE_FILE"), *out = getenv("PICO_ACE_ACE_DUMP");
    if (one && out) return dump_one(one, out);
    const char *dir = getenv("PICO_ACE_ACE_DIR");
    if (dir) return archive(dir);

    snap_ace_info_t in;

    /* ---- a 19K file loads, and its word runs ---------------------------- */
    CHECK(with_sq(&g, ACE_RAM_19K), "boot 19K");
    CHECK(g.m.cpu.pc == SNAP_ACE_WAIT_PC1 || g.m.cpu.pc == SNAP_ACE_WAIT_PC2,
          "the prompt is the key wait: PC %04X", g.m.cpu.pc);
    CHECK(g.m.cpu.sp == 0x7FFEu && ace_peek(&g.m, 0x7FFEu) == 0xF7u &&
          ace_peek(&g.m, 0x7FFFu) == 0x04u,
          "and its stack is the one word $04F7 at RAMTOP - 2 (snap_ace.h): SP %04X",
          g.m.cpu.sp);
    make(&g.m, 0x8000u, 0x8000u);

    CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot the other");
    guest_type(&h, "2 2 + .\n");
    CHECK(!sq_runs(&h), "control: a machine not loaded does not know SQ");
    CHECK(check(&h.m, &in) == SNAP_ACE_OK, "check: %s", snap_ace_status_str(check(&h.m, &in)));
    CHECK(in.needs == ACE_RAM_19K && in.end == 0x8000u && !in.repaired,
          "a 19K file to $7FFF: needs %s, ends %05X", ace_ram_name(in.needs), (unsigned)in.end);
    CHECK(load(&h.m, &in) == SNAP_ACE_OK, "load");
    CHECK(memcmp(h.m.vram, g.m.vram, ACE_BLOCK_BYTES) == 0 &&
          memcmp(h.m.cram, g.m.cram, ACE_BLOCK_BYTES) == 0 &&
          memcmp(h.m.uram, g.m.uram, ACE_BLOCK_BYTES) == 0 &&
          memcmp(h.m.xram, g.m.xram, ACE_XRAM_19K) == 0, "RAM as it was saved");
    CHECK(h.m.cpu.pc == g.m.cpu.pc && h.m.cpu.sp == g.m.cpu.sp && h.m.cpu.iy.w == g.m.cpu.iy.w &&
          h.m.cpu.hl_ == g.m.cpu.hl_ && h.m.cpu.im == 1 && h.m.cpu.iff1 && h.m.cpu.iff2,
          "the registers, without the words' noise");
    CHECK(sq_runs(&h), "the loaded word runs:\n%s", guest_row(&h.m, 22));

    /* Read a byte at a time and in big pieces: the same machine. */
    file.piece = 1;
    CHECK(check(&h.m, &in) == SNAP_ACE_OK, "a byte a read");
    file.piece = 4096;
    CHECK(check(&h.m, &in) == SNAP_ACE_OK, "4 KiB a read");
    file.piece = 0;

    /* ---- refused by the wrong machine, naming the one it needs ---------- */
    {
        static const ace_ram_t others[] = { ACE_RAM_3K, ACE_RAM_51K };
        for (unsigned i = 0; i < 2; i++) {
            CHECK(guest_boot(&h, others[i], 400), "boot %s", ace_ram_name(others[i]));
            ace_copy(&before, &h.m);
            CHECK(check(&h.m, &in) == SNAP_ACE_OTHER_RAM && in.needs == ACE_RAM_19K,
                  "a 19K file in the %s: %s, needs %s", ace_ram_name(others[i]),
                  snap_ace_status_str(check(&h.m, &in)), ace_ram_name(in.needs));
            CHECK(load(&h.m, &in) == SNAP_ACE_OTHER_RAM, "load refuses it too");
            CHECK(untouched(&before, &h.m), "and the %s is untouched", ace_ram_name(others[i]));
        }
    }

    /* ---- a 3K file, and one padded past RAM as six archive files are ----- */
    CHECK(with_sq(&g, ACE_RAM_3K), "boot 3K");
    make(&g.m, 0x4000u, 0x4000u);
    CHECK(guest_boot(&h, ACE_RAM_3K, 400), "boot 3K");
    CHECK(load(&h.m, &in) == SNAP_ACE_OK && in.needs == ACE_RAM_3K, "a 3K file: %s",
          snap_ace_status_str(load(&h.m, &in)));
    CHECK(sq_runs(&h), "runs in the 3K");
    lay_out(&g.m, 0x4000u, 0x8001u);
    memset(image + 0x4000u, 0x07, 0x4000u);
    encode(0x8001u);
    CHECK(guest_boot(&h, ACE_RAM_3K, 400), "boot 3K");
    CHECK(load(&h.m, &in) == SNAP_ACE_OK && in.end == 0x8001u,
          "ending at $8001 with $07 padding: %s, end %05X", snap_ace_status_str(load(&h.m, &in)),
          (unsigned)in.end);
    CHECK(sq_runs(&h), "runs in the 3K");

    /* ---- a 51K file without its stack: the key wait's word written back -- */
    CHECK(with_sq(&g, ACE_RAM_51K), "boot 51K");
    CHECK(g.m.cpu.sp == 0xFFFEu, "the 51K's stack is at the top: SP %04X", g.m.cpu.sp);
    make(&g.m, 0x0000u, 0x8000u);          /* dumped to $7FFF, as the archive's */
    CHECK(guest_boot(&h, ACE_RAM_51K, 400), "boot 51K");
    CHECK(check(&h.m, &in) == SNAP_ACE_OK && in.repaired && in.needs == ACE_RAM_51K,
          "a 51K file to $7FFF in the key wait: %s, repaired %d",
          snap_ace_status_str(check(&h.m, &in)), in.repaired);
    CHECK(load(&h.m, &in) == SNAP_ACE_OK, "load");
    CHECK(ace_peek(&h.m, 0xFFFEu) == 0xF7u && ace_peek(&h.m, 0xFFFFu) == 0x04u, "$04F7 at $FFFE");
    CHECK(sq_runs(&h), "runs in the 51K once repaired");

    /* Control: the same load with that word taken away again does not. */
    CHECK(load(&h.m, &in) == SNAP_ACE_OK, "load");
    ace_poke(&h.m, 0xFFFEu, 0);
    ace_poke(&h.m, 0xFFFFu, 0);
    CHECK(!sq_runs(&h), "control: without $04F7 the ROM does not come back to the prompt");

    /* A 35K file, made from it: RAMTOP $C000 in the ROM's variable and in
     * ACE32's word, the stack at $BFFE, dumped to $7FFF. */
    {
        CHECK(with_sq(&g, ACE_RAM_51K), "boot 51K");
        ace_poke(&g.m, 0x3C18u, 0x00);
        ace_poke(&g.m, 0x3C19u, 0xC0);
        g.m.cpu.sp = 0xBFFEu;
        make(&g.m, 0xC000u, 0x8000u);
        CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot 19K");
        ace_copy(&before, &h.m);
        CHECK(check(&h.m, &in) == SNAP_ACE_OTHER_RAM && in.needs == ACE_RAM_51K &&
              strcmp(snap_ace_taken_on(&in), "35K") == 0,
              "a 35K file in the 19K: %s, needs %s, taken on %s",
              snap_ace_status_str(check(&h.m, &in)), ace_ram_name(in.needs),
              snap_ace_taken_on(&in));
        CHECK(untouched(&before, &h.m), "and the 19K is untouched");
        CHECK(guest_boot(&h, ACE_RAM_51K, 400), "boot 51K");
        CHECK(load(&h.m, &in) == SNAP_ACE_OK && in.repaired, "the 51K loads it, repaired");
        CHECK(ace_peek(&h.m, 0xBFFEu) == 0xF7u && ace_peek(&h.m, 0xBFFFu) == 0x04u,
              "$04F7 at $BFFE");
        CHECK(sq_runs(&h), "and it runs");

        /* Not in the key wait, the missing stack cannot be written back. */
        g.m.cpu.pc = 0x059Fu;
        make(&g.m, 0xC000u, 0x8000u);
        ace_copy(&before, &h.m);
        CHECK(check(&h.m, &in) == SNAP_ACE_NO_STACK, "outside the key wait: %s",
              snap_ace_status_str(check(&h.m, &in)));
        g.m.cpu.pc = SNAP_ACE_WAIT_PC1;
        g.m.cpu.hl.w = 0x3C29u;
        make(&g.m, 0xC000u, 0x8000u);
        CHECK(check(&h.m, &in) == SNAP_ACE_NO_STACK, "HL not FLAGS: %s",
              snap_ace_status_str(check(&h.m, &in)));
        CHECK(untouched(&before, &h.m), "the checks leave the 51K untouched");
    }

    /* ---- files that are not .ace ------------------------------------------- *
     * The check finds each; the load alone refuses only what the header
     * shows, before it writes (snap_ace.h). */
    CHECK(with_sq(&g, ACE_RAM_19K), "boot 19K");
    CHECK(guest_boot(&h, ACE_RAM_19K, 400), "boot 19K");
    ace_copy(&before, &h.m);
    make(&g.m, 0x8000u, 0x8000u);
    file.len -= 2;
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "no end mark: %s", snap_ace_status_str(check(&h.m, &in)));
    file.len += 2;
    file.fail_at = file.len / 2u;
    CHECK(check(&h.m, &in) == SNAP_ACE_IO, "a read error: %s", snap_ace_status_str(check(&h.m, &in)));
    file.fail_at = 0;
    make(&g.m, 0x8000u, 0x3F00u);
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "short of $4000: %s", snap_ace_status_str(check(&h.m, &in)));
    make(&g.m, 0x7000u, 0x8000u);
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "RAMTOP $7000: %s", snap_ace_status_str(check(&h.m, &in)));
    CHECK(load(&h.m, &in) == SNAP_ACE_NOT_ACE, "load: RAMTOP $7000");
    lay_out(&g.m, 0x8000u, 0x8000u);
    image[0x2130u] = 3;
    encode(0x8000u);
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "IM 3: %s", snap_ace_status_str(check(&h.m, &in)));
    CHECK(load(&h.m, &in) == SNAP_ACE_NOT_ACE, "load: IM 3");
    lay_out(&g.m, 0x8000u, 0x8000u);
    encode(0x10200u);                      /* past the end of memory */
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "too long: %s", snap_ace_status_str(check(&h.m, &in)));
    file.len = 0;
    CHECK(check(&h.m, &in) == SNAP_ACE_NOT_ACE, "empty: %s", snap_ace_status_str(check(&h.m, &in)));
    CHECK(untouched(&before, &h.m), "none of them changed the machine");

    /* Not while the CPU is stalled on a tape call. */
    make(&g.m, 0x8000u, 0x8000u);
    h.m.tape.op = TAPE_LOAD;
    CHECK(check(&h.m, &in) == SNAP_ACE_BUSY && load(&h.m, &in) == SNAP_ACE_BUSY, "busy");
    h.m.tape.op = TAPE_NONE;

    TEST_DONE();
}
