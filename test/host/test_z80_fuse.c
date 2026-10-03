/* test_z80_fuse.c — FUSE's Z80 tests (design.md §5.4): the cycle table
 * asserted by execution.
 *
 * Every test in tests.in is run from its stated state until at least its
 * T-states have passed, as FUSE's own harness does, and checked against
 * tests.expected on every register (MEMPTR, I, R, IFF1, IFF2, IM and the
 * halt state included), on the T-states, on all 64 KiB of memory, and on
 * the order of memory and port accesses with their addresses and data.
 *
 * Not checked: the time of each access within an instruction, and the
 * contention (MC, PC) events. This emulator is exact per instruction, not
 * per T-state (§5.1).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "test_util.h"
#include "z80.h"

#define MAX_EVENTS 512

typedef struct {
    char     kind;          /* 'R' 'W' 'I' 'O': memory read/write, port in/out */
    uint16_t addr;
    uint8_t  data;
} event_t;

static uint8_t mem[65536];
static uint8_t want_mem[65536];
static event_t got[MAX_EVENTS], want[MAX_EVENTS];
static int     ngot, nwant;

static void log_event(char kind, uint16_t addr, uint8_t data) {
    if (ngot < MAX_EVENTS)
        got[ngot] = (event_t){ kind, addr, data };
    ngot++;
}

/* Every page is NULL, so every access comes here and is logged. */
static uint8_t mem_read(void *ctx, uint16_t a) {
    (void)ctx;
    log_event('R', a, mem[a]);
    return mem[a];
}

static void mem_write(void *ctx, uint16_t a, uint8_t v) {
    (void)ctx;
    log_event('W', a, v);
    mem[a] = v;
}

/* FUSE's harness answers a port read with the port's high byte. */
static uint8_t io_read(void *ctx, uint16_t port) {
    (void)ctx;
    uint8_t v = (uint8_t)(port >> 8);
    log_event('I', port, v);
    return v;
}

static void io_write(void *ctx, uint16_t port, uint8_t v) {
    (void)ctx;
    log_event('O', port, v);
}

static const page_t no_pages[Z80_PAGE_COUNT];   /* all NULL */

/* FUSE fills memory with DE AD BE EF before loading a test. */
static void fill_pattern(uint8_t *m) {
    static const uint8_t pat[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    for (unsigned i = 0; i < 65536u; i++)
        m[i] = pat[i & 3];
}

/* A test's state, in the order FUSE writes it. */
enum { AF, BC, DE, HL, AF_, BC_, DE_, HL_, IX, IY, SP, PC, MEMPTR,
       I, R, IFF1, IFF2, IM, HALTED, TSTATES, NFIELDS };
typedef struct { unsigned v[NFIELDS]; } state_t;

static bool read_line(FILE *f, char *buf, size_t n) {
    if (!fgets(buf, (int)n, f))
        return false;
    buf[strcspn(buf, "\r\n")] = 0;
    return true;
}

static bool parse_state(FILE *f, state_t *s) {
    char l1[256], l2[256];
    if (!read_line(f, l1, sizeof l1) || !read_line(f, l2, sizeof l2))
        return false;
    unsigned *v = s->v;
    return sscanf(l1, "%x %x %x %x %x %x %x %x %x %x %x %x %x", &v[AF], &v[BC], &v[DE],
                  &v[HL], &v[AF_], &v[BC_], &v[DE_], &v[HL_], &v[IX], &v[IY], &v[SP],
                  &v[PC], &v[MEMPTR]) == 13 &&
           sscanf(l2, "%x %x %u %u %u %u %u", &v[I], &v[R], &v[IFF1], &v[IFF2], &v[IM],
                  &v[HALTED], &v[TSTATES]) == 7;
}

/* "addr byte byte ... -1" into m. */
static void apply_mem_line(const char *line, uint8_t *m) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", line);
    char *tok = strtok(buf, " \t");
    if (!tok)
        return;
    unsigned addr = (unsigned)strtoul(tok, NULL, 16);
    while ((tok = strtok(NULL, " \t")) && strcmp(tok, "-1") != 0)
        m[addr++ & 0xFFFFu] = (uint8_t)strtoul(tok, NULL, 16);
}

static void set_state(z80_t *c, const state_t *s) {
    const unsigned *v = s->v;
    c->af.w = (uint16_t)v[AF];
    c->bc.w = (uint16_t)v[BC];
    c->de.w = (uint16_t)v[DE];
    c->hl.w = (uint16_t)v[HL];
    c->af_ = (uint16_t)v[AF_];
    c->bc_ = (uint16_t)v[BC_];
    c->de_ = (uint16_t)v[DE_];
    c->hl_ = (uint16_t)v[HL_];
    c->ix.w = (uint16_t)v[IX];
    c->iy.w = (uint16_t)v[IY];
    c->sp = (uint16_t)v[SP];
    c->pc = (uint16_t)v[PC];
    c->wz.w = (uint16_t)v[MEMPTR];
    c->i = (uint8_t)v[I];
    c->r = c->r7 = (uint8_t)v[R];
    c->iff1 = (uint8_t)v[IFF1];
    c->iff2 = (uint8_t)v[IFF2];
    c->im = (uint8_t)v[IM];
    c->halted = v[HALTED] != 0;
}

static void get_state(const z80_t *c, state_t *s) {
    *s = (state_t){ {
        c->af.w, c->bc.w, c->de.w, c->hl.w, c->af_, c->bc_, c->de_, c->hl_,
        c->ix.w, c->iy.w, c->sp, c->pc, c->wz.w,
        c->i, z80_r(c), c->iff1, c->iff2, c->im, c->halted, c->t,
    } };
}

static const char *const field_names[NFIELDS] = {
    "AF", "BC", "DE", "HL", "AF'", "BC'", "DE'", "HL'", "IX", "IY", "SP", "PC", "MEMPTR",
    "I", "R", "IFF1", "IFF2", "IM", "halted", "T",
};

/* Runs one test; prints what differs and returns false if anything does. */
static bool run_one(const char *name, const state_t *in, const state_t *exp) {
    z80_t c;
    memset(&c, 0, sizeof c);
    c.bus = (z80_bus_t){ no_pages, NULL, mem_read, mem_write, io_read, io_write };
    z80_reset(&c);
    set_state(&c, in);
    c.t = 0;
    ngot = 0;

    z80_run(&c, in->v[TSTATES]);

    bool ok = true;
    state_t s;
    get_state(&c, &s);
    for (unsigned k = 0; k < NFIELDS; k++) {
        if (s.v[k] != exp->v[k]) {
            fprintf(stderr, "%s: %s is %04x, expected %04x\n", name, field_names[k], s.v[k],
                    exp->v[k]);
            ok = false;
        }
    }

    for (unsigned a = 0; a < 65536u; a++) {
        if (mem[a] != want_mem[a]) {
            fprintf(stderr, "%s: memory %04x is %02x, expected %02x\n", name, a, mem[a],
                    want_mem[a]);
            ok = false;
            break;
        }
    }

    int n = ngot < nwant ? ngot : nwant;
    for (int k = 0; k < n; k++) {
        if (got[k].kind != want[k].kind || got[k].addr != want[k].addr ||
            got[k].data != want[k].data) {
            fprintf(stderr, "%s: access %d is %c %04x %02x, expected %c %04x %02x\n", name, k,
                    got[k].kind, got[k].addr, got[k].data, want[k].kind, want[k].addr,
                    want[k].data);
            ok = false;
            break;
        }
    }
    if (ngot != nwant) {
        fprintf(stderr, "%s: %d accesses, expected %d\n", name, ngot, nwant);
        ok = false;
    }
    return ok;
}

int main(void) {
    FILE *fin = suite_open("tests.in", "r");
    FILE *fexp = suite_open("tests.expected", "r");

    static uint8_t init_mem[65536];
    char line[1024], name[sizeof line];
    int tests = 0, failed = 0;

    for (;;) {
        /* tests.in: name, two state lines, memory lines up to "-1". */
        do {
            if (!read_line(fin, line, sizeof line))
                goto done;
        } while (line[0] == 0);
        snprintf(name, sizeof name, "%s", line);

        state_t in, exp;
        CHECK(parse_state(fin, &in), "%s: bad state in tests.in", name);
        fill_pattern(init_mem);
        while (read_line(fin, line, sizeof line) && strcmp(line, "-1") != 0)
            apply_mem_line(line, init_mem);

        /* tests.expected: the same name, events (indented), two state
         * lines, then changed memory up to a blank line. */
        do {
            CHECK(read_line(fexp, line, sizeof line), "tests.expected ends before %s", name);
        } while (line[0] == 0);
        CHECK(strcmp(line, name) == 0, "tests.expected has %s where %s was expected", line,
              name);

        nwant = 0;
        long pos = ftell(fexp);
        while (read_line(fexp, line, sizeof line) && (line[0] == ' ' || line[0] == '\t')) {
            unsigned t, addr, data = 0;
            char kind[4];
            int k = sscanf(line, "%u %3s %x %x", &t, kind, &addr, &data);
            CHECK(k >= 3, "%s: bad event line '%s'", name, line);
            char e = 0;
            if (strcmp(kind, "MR") == 0) e = 'R';
            else if (strcmp(kind, "MW") == 0) e = 'W';
            else if (strcmp(kind, "PR") == 0) e = 'I';
            else if (strcmp(kind, "PW") == 0) e = 'O';
            if (e && nwant < MAX_EVENTS)
                want[nwant++] = (event_t){ e, (uint16_t)addr, (uint8_t)data };
            pos = ftell(fexp);
        }
        fseek(fexp, pos, SEEK_SET);
        CHECK(parse_state(fexp, &exp), "%s: bad state in tests.expected", name);

        memcpy(want_mem, init_mem, sizeof want_mem);
        while (read_line(fexp, line, sizeof line) && line[0] != 0)
            apply_mem_line(line, want_mem);

        memcpy(mem, init_mem, sizeof mem);
        tests++;
        if (!run_one(name, &in, &exp))
            failed++;
    }

done:
    fclose(fin);
    fclose(fexp);
    printf("%d FUSE tests, %d failed\n", tests, failed);
    CHECK(tests > 1000, "only %d tests read; is tests.in complete?", tests);
    CHECK(failed == 0, "%d of %d tests failed", failed, tests);
    TEST_DONE();
}
