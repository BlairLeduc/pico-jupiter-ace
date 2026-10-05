/* test_field.c — the field and its split at INT's edges (design.md §5.3,
 * §11.1, §15.2 M3). Each rule beside a control that must fail. */

#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "guest.h"
#include "test_util.h"

static ace_t m;

#define REDRAW_BYTES 512u

/* A guest that waits for INT with HALT, then redraws the top 16 rows
 * with the interrupt count, kept at $5000, so a snapshot shows one value
 * there only if the redraw had finished (§11.1). The rows are what fits
 * between INT and the display, 64 lines or 13,312 T; the whole screen,
 * 16,107 T, would tear on an Ace too. IM 2, since the
 * ROM's own handler is at $0038: with I = $3C and $FF on the bus the
 * vector is read from $3CFF and $3D00. */
static const uint8_t redraw_main[] = {          /* at $4100 */
    0xF3,                   /* DI                                 */
    0xED, 0x5E,             /* IM 2                               */
    0x3E, 0x3C,             /* LD A,$3C                           */
    0xED, 0x47,             /* LD I,A                             */
    0xFB,                   /* loop: EI                           */
    0x76,                   /* HALT                               */
    0x21, 0x00, 0x24,       /* LD HL,$2400                        */
    0x11, 0x01, 0x24,       /* LD DE,$2401                        */
    0x01, 0xFF, 0x01,       /* LD BC,511                          */
    0x3A, 0x00, 0x50,       /* LD A,($5000)                       */
    0x77,                   /* LD (HL),A                          */
    0xED, 0xB0,             /* LDIR: 10,726 T                     */
    0x18, 0xED,             /* JR loop                            */
};
/* Returns with interrupts off: an EI here would take INT again while it
 * is still held, as the ROM's handler avoids by waiting it out (§5.3). */
static const uint8_t redraw_isr[] = {           /* at $4000 */
    0xF5,                   /* PUSH AF                            */
    0x3A, 0x00, 0x50,       /* LD A,($5000)                       */
    0x3C,                   /* INC A                              */
    0x32, 0x00, 0x50,       /* LD ($5000),A                       */
    0xF1,                   /* POP AF                             */
    0xED, 0x4D,             /* RETI                               */
};

static bool redraw_boot(const ace_config_t *cfg) {
    if (!ace_init(&m, cfg)) return false;
    memcpy(&m.xram[0x0100], redraw_main, sizeof redraw_main);
    memcpy(&m.xram[0x0000], redraw_isr, sizeof redraw_isr);
    m.uram[0x3CFFu & 0x3FFu] = 0x00;      /* the vector: $4000 */
    m.uram[0x3D00u & 0x3FFu] = 0x40;
    m.cpu.pc = 0x4100;
    m.cpu.sp = 0x8000;                    /* reset's $FFFF is unpopulated */
    return true;
}

/* Fields, of 100, whose snapshot shows a torn redraw. */
static int torn_fields(void) {
    int torn = 0;
    for (int f = 0; f < 100; f++) {
        ace_run_field(&m);
        const uint8_t *s = ace_screen(&m);
        for (unsigned i = 1; i < REDRAW_BYTES; i++) {
            if (s[i] != s[0]) { torn++; break; }
        }
    }
    return torn;
}

int main(void) {
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;

    /* ---- The shape: 312 lines of 208 T, split at INT's edges. */
    CHECK(ace_init(&m, &cfg), "ace_init refused the defaults");
    CHECK(m.field_t[0] == 248u * 208u, "active display to INT: %u T", m.field_t[0]);
    CHECK(m.field_t[1] == 8u * 208u, "INT held %u T", m.field_t[1]);
    CHECK(ace_field_t(&m) == 64896u, "field %u T", ace_field_t(&m));

    /* ---- Debt: whole instructions, and the overshoot paid back, so N
     * fields run N x 64,896 T to within one instruction. */
    uint64_t total = 0;
    for (int f = 0; f < 1000; f++) total += ace_run_field(&m);
    int64_t over = (int64_t)total - 1000LL * 64896;
    CHECK(over >= 0 && over < 23, "1,000 fields ran %lld T over", (long long)over);
    CHECK(over == -m.budget, "the overshoot %lld is not the debt %d", (long long)over, m.budget);
    CHECK(!m.cpu.int_line, "a field ended with INT asserted");

    /* ---- A redraw after INT is finished by the field's end (§11.1). */
    CHECK(redraw_boot(&cfg), "redraw machine");
    ace_run_field(&m);                    /* to the first HALT and INT */
    uint8_t before = m.xram[0x1000];
    int torn = torn_fields();
    CHECK(torn == 0, "%d of 100 snapshots tore", torn);
    CHECK((uint8_t)(m.xram[0x1000] - before) == 100,
          "%u interrupts in 100 fields", (uint8_t)(m.xram[0x1000] - before));

    /* The control: a field that ends one line after INT rises, the
     * single-point split, catches every redraw half done. */
    ace_config_t one = cfg;
    one.active_line = one.int_line + 1;
    one.int_t = one.line_t;
    CHECK(redraw_boot(&one), "single-point machine");
    ace_run_field(&m);
    torn = torn_fields();
    CHECK(torn == 100, "the single-point control tore only %d of 100", torn);

    /* ---- INT's duration, against the ROM. Its handler reaches EI about
     * 1,800 T after INT rises (ROM $0038-$017C, timed 2026-10-03), so INT
     * held longer is taken twice and FRAMES ($3C2B) counts two a field. */
    static guest_t g;
    CHECK(guest_boot(&g, ACE_RAM_19K, 500), "the ROM did not boot");
    uint16_t f0 = guest_sysvar16(&g.m, 0x3C2B);
    guest_fields(&g, 500);
    uint16_t counted = (uint16_t)(guest_sysvar16(&g.m, 0x3C2B) - f0);
    CHECK(counted == 500, "FRAMES counted %u in 500 fields", counted);

    /* The control: INT held 2,500 T, in a field of the same length, is
     * taken again. */
    g.m.field_t[1] = 2500u;
    g.m.field_t[2] = 64896u - g.m.field_t[0] - 2500u;
    f0 = guest_sysvar16(&g.m, 0x3C2B);
    guest_fields(&g, 100);
    counted = (uint16_t)(guest_sysvar16(&g.m, 0x3C2B) - f0);
    CHECK(counted == 200, "with INT held 2,500 T, FRAMES counted %u in 100 fields", counted);

    TEST_DONE();
}
