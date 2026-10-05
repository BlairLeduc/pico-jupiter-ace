/* test_wait.c — the waiting mirrors' hold (design.md §6.4, §15.2 M14).
 *
 * The circuit holds WAIT for a memory access to $2000-$2FFF with A10
 * high while VIDEN is up: the first 128 T of each of the display's 192
 * lines. The CPU's clock is the pixel counter's CNT0, so the hold is a
 * fixed function of the T-state in the field. Each case below runs one
 * instruction at a chosen place in the field and counts its T-states;
 * the model switched off is the control.
 *
 * Then the measurement M14 asks for, printed: the ROM printing a
 * screenful and listing its dictionary, with the hold and without. With
 * PICO_ACE_WAIT_ACE=file.ace (and PICO_ACE_WAIT_KEYS, typed after it
 * loads and its input line is cleared), a game's loop too: instructions
 * run per field, both ways; PICO_ACE_WAIT_DUMP=1 shows the screen.
 * M14 ran dreamsoft racer with "GO\n" and Pacman with "RUN\n".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "guest.h"
#include "keymatrix.h"
#include "snap_ace.h"
#include "test_util.h"

static ace_t m;

/* Run the instruction at $4000 with the field `pos` T-states old, and
 * return the T-states it took. */
static uint32_t one_at(uint32_t pos, const uint8_t *code, size_t len) {
    memcpy(m.xram, code, len);
    m.cpu.pc = 0x4000;
    m.cpu.sp = 0x8000;
    m.field_start = m.cpu.t - pos;
    uint32_t t0 = m.cpu.t;
    ace_run(&m, 1);
    return m.cpu.t - t0;
}

#define AT(line, h) ((uint32_t)(line) * 208u + (uint32_t)(h))

static const uint8_t st_2400[] = { 0x32, 0x00, 0x24 };   /* LD ($2400),A  13 T */
static const uint8_t st_2000[] = { 0x32, 0x00, 0x20 };   /* LD ($2000),A       */
static const uint8_t st_2c00[] = { 0x32, 0x00, 0x2C };   /* LD ($2C00),A       */
static const uint8_t st_2800[] = { 0x32, 0x00, 0x28 };   /* LD ($2800),A       */
static const uint8_t st_3c00[] = { 0x32, 0x00, 0x3C };   /* LD ($3C00),A       */
static const uint8_t ld_2700[] = { 0x3A, 0xFF, 0x27 };   /* LD A,($27FF)  13 T */
static const uint8_t in_fe[]   = { 0xDB, 0xFE };         /* IN A,($FE)    11 T */
static const uint8_t jp_2500[] = { 0xC3, 0x00, 0x25 };   /* JP $2500      10 T */

/* ---- The measurement ---------------------------------------------------- */

static guest_t g;

/* At the key wait with the cursor on screen: the ROM has finished. */
static bool at_prompt(const ace_t *a) {
    uint16_t pc = a->cpu.pc;
    return pc == SNAP_ACE_WAIT_PC1 || pc == SNAP_ACE_WAIT_PC2;
}

/* ace_run_field an instruction at a time, as guest_boot times the boot:
 * the same field start, budget and INT edges. The T-states to the first
 * instruction at the key wait, or 0 if the field ends first. */
static uint64_t steps_to_prompt(ace_t *a) {
    uint64_t t = 0;
    a->field_start = a->cpu.t + (uint32_t)a->budget;
    int32_t budget = a->budget;
    for (int part = 0; part < 3; part++) {
        budget += (int32_t)a->field_t[part];
        while (budget > 0) {
            uint32_t d = ace_run(a, 1);
            budget -= (int32_t)d;
            t += d;
            if (at_prompt(a)) return t;
        }
        z80_set_int(&a->cpu, part == 0);
    }
    return 0;
}

/* Type `line`, then time it from the field Enter is queued in until the
 * ROM is back at the key wait, to the instruction, with the T-states held
 * on the way. Enter's replay is the same both ways, so the difference is
 * the job's. */
typedef struct { uint64_t t; uint32_t held; } job_t;

static job_t job(bool wait, const char *define, const char *line) {
    static ace_t before;
    job_t j = { 0, 0 };
    if (!guest_boot(&g, ACE_RAM_19K, 400)) return j;
    ace_set_wait_states(&g.m, wait);
    if (define) guest_type(&g, define);
    guest_type(&g, line);
    uint32_t w0 = g.m.wait_t;
    picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
    unsigned n = keymap_picocalc_text('\n', ev);
    for (unsigned i = 0; i < n; i++) keymatrix_event(&g.k, ev[i].state, ev[i].code);
    /* Enter's replay, and a field for the ROM to take it. */
    while (!keymatrix_idle(&g.k)) j.t += (guest_fields(&g, 1), ace_field_t(&g.m));
    j.t += (guest_fields(&g, 1), ace_field_t(&g.m));
    for (int f = 0; f < 20000 && !at_prompt(&g.m); f++) {
        keymatrix_field(&g.k, &g.m);
        ace_copy(&before, &g.m);
        uint32_t done = ace_run_field(&g.m);
        if (!at_prompt(&g.m)) { j.t += done; continue; }
        j.t += steps_to_prompt(&before);
        j.held = before.wait_t - w0;
        return j;
    }
    return (job_t){ 0, 0 };
}

static void report(const char *what, job_t off, job_t on) {
    printf("%-10s %9llu T without, %9llu with (%+.1f %%); held %u T\n", what,
           (unsigned long long)off.t, (unsigned long long)on.t,
           off.t ? 100.0 * ((double)on.t - (double)off.t) / (double)off.t : 0.0, on.held);
}

static uint8_t *s_file;
static size_t   s_size, s_pos;

static int file_read(void *ctx, uint8_t *dst, size_t max) {
    (void)ctx;
    size_t n = s_size - s_pos < max ? s_size - s_pos : max;
    memcpy(dst, s_file + s_pos, n);
    s_pos += n;
    return (int)n;
}

/* A game from the archive: load, type its start, then count what runs
 * over 500 fields. Returns instructions per field. */
static double game(const char *path, const char *keys, bool wait, uint32_t *held) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t buf[65536];
    s_size = fread(buf, 1, sizeof buf, f);
    fclose(f);
    s_file = buf;
    snap_ace_info_t in;
    if (!guest_boot(&g, ACE_RAM_19K, 400)) return 0;
    s_pos = 0;
    if (snap_ace_check(&g.m, file_read, NULL, &in) == SNAP_ACE_OTHER_RAM &&
        !guest_boot(&g, in.needs, 400)) return 0;
    s_pos = 0;
    if (snap_ace_load(&g.m, file_read, NULL, &in) != SNAP_ACE_OK) return 0;
    keymatrix_init(&g.k);
    ace_set_wait_states(&g.m, wait);
    guest_fields(&g, 10);
    if (keys) {
        guest_press(&g, 'X', true);        /* Alt+X, DELETE LINE: the file's input line */
        guest_type(&g, keys);
    }
    guest_fields(&g, 100);
    uint32_t i0 = g.m.cpu.insns, w0 = g.m.wait_t;
    guest_fields(&g, 500);
    *held = g.m.wait_t - w0;
    if (getenv("PICO_ACE_WAIT_DUMP")) guest_dump(&g.m, stdout);
    return (g.m.cpu.insns - i0) / 500.0;
}

int main(void) {
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    cfg.wait_states = true;
    CHECK(ace_init(&m, &cfg), "ace_init");

    /* ---- Where the hold falls: VIDEN, the first 128 T of lines 0-191. */
    CHECK(one_at(AT(0, 0), st_2400, 3) == 13 + 128, "at line 0, T 0");
    CHECK(one_at(AT(0, 100), st_2400, 3) == 13 + 28, "at T 100");
    CHECK(one_at(AT(0, 127), st_2400, 3) == 13 + 1, "at T 127");
    CHECK(one_at(AT(0, 128), st_2400, 3) == 13, "at T 128, VIDEN down");
    CHECK(one_at(AT(0, 207), st_2400, 3) == 13, "at T 207");
    CHECK(one_at(AT(191, 5), st_2400, 3) == 13 + 123, "on line 191");
    CHECK(one_at(AT(192, 0), st_2400, 3) == 13, "on line 192, below the display");
    CHECK(one_at(AT(250, 0), st_2400, 3) == 13, "in INT's lines");
    CHECK(one_at(AT(311, 207), st_2400, 3) == 13, "at the field's last T");
    CHECK(one_at(AT(312, 10), st_2400, 3) == 13 + 118, "a field late, folded");

    /* Which accesses: memory at $2400-$27FF and $2C00-$2FFF, read, write
     * and opcode fetch; not the CPU-priority mirrors, user RAM or I/O. */
    CHECK(one_at(AT(0, 0), st_2c00, 3) == 13 + 128, "character RAM's waiting mirror");
    CHECK(one_at(AT(0, 0), ld_2700, 3) == 13 + 128, "a read of $27FF");
    CHECK(one_at(AT(0, 0), st_2000, 3) == 13, "$2000 has the CPU's priority");
    CHECK(one_at(AT(0, 0), st_2800, 3) == 13, "$2800 has the CPU's priority");
    CHECK(one_at(AT(0, 0), st_3c00, 3) == 13, "user RAM");
    CHECK(one_at(AT(0, 0), in_fe, 2) == 11, "an IN");
    CHECK(one_at(AT(0, 0), jp_2500, 3) == 10, "a JP to $2500");
    m.vram[0x100] = 0x00;                       /* NOP at $2500 */
    uint32_t t0 = m.cpu.t;
    m.field_start = t0;
    ace_run(&m, 1);
    CHECK(m.cpu.t - t0 == 4 + 128, "an opcode fetch from $2500 took %u T", m.cpu.t - t0);

    /* The data still arrives, both mirrors one RAM. */
    m.cpu.af.b.h = 0x5A;
    one_at(AT(0, 0), st_2400, 3);
    CHECK(ace_peek(&m, 0x2000) == 0x5A && m.vram[0] == 0x5A, "the held write was lost");
    m.cpu.af.b.h = 0xA5;
    one_at(AT(0, 0), st_2c00, 3);
    CHECK(m.cram[0] == 0xA5, "the held character write was lost");
    m.cpu.af.b.h = 0;
    one_at(AT(300, 0), ld_2700, 3);
    CHECK(m.cpu.af.b.h == m.vram[0x3FF], "a read of the waiting mirror");

    /* The control: the model off holds nothing, anywhere. */
    ace_set_wait_states(&m, false);
    CHECK(one_at(AT(0, 0), st_2400, 3) == 13, "held with the model off");
    CHECK(one_at(AT(0, 0), st_2c00, 3) == 13, "held with the model off");
    ace_set_wait_states(&m, true);

    /* ---- ace_run_field places the field: an access at its first T is
     * held the whole of VIDEN, whatever debt the last field left. */
    for (int k = 0; k < 3; k++) {
        ace_run_field(&m);
        CHECK(m.field_start == m.cpu.t + (uint32_t)m.budget - ace_field_t(&m),
              "field_start is not the field's nominal start");
    }

    /* ---- The real ROM boots and computes with the hold. */
    CHECK(guest_boot(&g, ACE_RAM_19K, 400), "boot");
    ace_set_wait_states(&g.m, true);
    guest_type(&g, "2 2 + .\n");
    CHECK(guest_screen_has(&g.m, "2 2 + . 4  OK"), "the ROM with the hold did not add");
    CHECK(g.m.wait_t > 0, "the ROM was never held");

    /* ---- The measurement (§15.2 M14). */
    const char *def = ": S 24 0 DO CR .\" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123\" LOOP ;\n";
    job_t s_off = job(false, def, "S"), s_on = job(true, def, "S");
    report("screenful", s_off, s_on);
    CHECK(s_off.held == 0 && s_on.held > 0, "the screenful's hold");
    CHECK(s_on.t > s_off.t, "the hold made no difference to the screenful");

    job_t v_off = job(false, NULL, "VLIST"), v_on = job(true, NULL, "VLIST");
    report("VLIST", v_off, v_on);

    const char *ace = getenv("PICO_ACE_WAIT_ACE");
    if (ace) {
        uint32_t h_off, h_on;
        double off = game(ace, getenv("PICO_ACE_WAIT_KEYS"), false, &h_off);
        double on  = game(ace, getenv("PICO_ACE_WAIT_KEYS"), true, &h_on);
        printf("%s: %.0f instructions a field without, %.0f with (%+.1f %%); held %.1f %% of the time\n",
               ace, off, on, off ? 100.0 * (on - off) / off : 0.0, 100.0 * h_on / (500.0 * 64896.0));
    }

    TEST_DONE();
}
