/* test_bus.c — the Ace's memory map and even port, through the CPU
 * (design.md §6, §13.2).
 *
 * Every access is made by a Z80 instruction run from the workspace at
 * $2700, so the page table, the slow path and the port decode are what
 * is tested, not the arrays behind them. */

#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "test_util.h"

static ace_t m;

static bool boot(ace_ram_t ram) {
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.ram = ram;
    cfg.rom = ace_rom;
    return ace_init(&m, &cfg);
}

/* Run `code` from $2700 with BC = bc until its HALT; return A. */
#define CODE_AT 0x2700u
static uint8_t run(const uint8_t *code, size_t n, uint16_t bc) {
    memcpy(&m.vram[CODE_AT - 0x2400u], code, n);
    m.vram[CODE_AT - 0x2400u + n] = 0x76u;                 /* HALT */
    m.cpu.pc = CODE_AT;
    m.cpu.iff1 = m.cpu.iff2 = 0;
    m.cpu.halted = false;
    m.cpu.bc.w = bc;
    for (int i = 0; i < 100 && !m.cpu.halted; i++) z80_step(&m.cpu);
    return m.cpu.af.b.h;
}

static void poke(uint16_t a, uint8_t v) {                  /* LD A,v; LD (a),A */
    const uint8_t c[] = { 0x3E, v, 0x32, (uint8_t)a, (uint8_t)(a >> 8) };
    run(c, sizeof c, 0);
}

static uint8_t peek(uint16_t a) {                          /* LD A,(a) */
    const uint8_t c[] = { 0x3A, (uint8_t)a, (uint8_t)(a >> 8) };
    return run(c, sizeof c, 0);
}

static uint8_t in_port(uint16_t port) {                    /* IN A,(C) */
    const uint8_t c[] = { 0xED, 0x78 };
    return run(c, sizeof c, port);
}

static void out_port(uint16_t port, uint8_t v) {           /* LD A,v; OUT (C),A */
    const uint8_t c[] = { 0x3E, v, 0xED, 0x79 };
    run(c, sizeof c, port);
}

int main(void) {
    CHECK(boot(ACE_RAM_19K), "ace_init refused the defaults");
    CHECK(ace_field_t(&m) == 64896u, "field %u T, expected 312 x 208", ace_field_t(&m));

    /* ---- Video RAM: $2000 and $2400 are one 1 KiB (§2.2). */
    for (unsigned off = 0; off < ACE_BLOCK_BYTES; off += 0x55u) {
        if (0x2000u + off >= 0x2300u && 0x2000u + off < 0x2400u) continue;  /* our code's mirror */
        uint8_t v = (uint8_t)(off ^ 0xA5u);
        poke((uint16_t)(0x2000u + off), v);
        CHECK(peek((uint16_t)(0x2400u + off)) == v, "video $%04X not at $%04X",
              0x2000u + off, 0x2400u + off);
    }
    poke(0x2401, 0x42);
    CHECK(ace_screen(&m)[1] == 0x42, "ace_screen is not $2400");

    /* ---- Character RAM: written through both mirrors, read by neither. */
    poke(0x2800, 0x3C);
    poke(0x2C07, 0x81);
    CHECK(ace_charset(&m)[0] == 0x3C && ace_charset(&m)[7] == 0x81,
          "character RAM writes missed: $%02X $%02X", ace_charset(&m)[0], ace_charset(&m)[7]);
    CHECK(peek(0x2C00) == m.cfg.cram_read && peek(0x2800) == m.cfg.cram_read,
          "character RAM read back $%02X, $%02X", peek(0x2C00), peek(0x2800));
    /* The control: what the reads returned is not just what was written. */
    CHECK(m.cfg.cram_read != 0x3C, "the test cannot tell a read-back from cram_read");

    /* ---- User RAM: one 1 KiB, four times over $3000-$3FFF. */
    for (unsigned off = 0; off < ACE_BLOCK_BYTES; off += 0x77u) {
        uint8_t v = (uint8_t)(off + 0x11u);
        poke((uint16_t)(0x3000u + off), v);
        for (unsigned w = 0x3400u; w <= 0x3C00u; w += 0x400u)
            CHECK(peek((uint16_t)(w + off)) == v, "user RAM $%04X not at $%04X",
                  0x3000u + off, w + off);
    }

    /* ---- ROM: reads the image, ignores writes. */
    CHECK(peek(0x0000) == 0xF3, "ROM $0000 reads $%02X, not DI", peek(0x0000));
    poke(0x0000, 0x00);
    poke(0x1FFF, (uint8_t)~ace_rom[0x1FFF]);
    CHECK(peek(0x0000) == 0xF3 && peek(0x1FFF) == ace_rom[0x1FFF], "a ROM write landed");

    /* ---- Expansion RAM and the open bus, in each machine (§6.2). */
    static const struct { ace_ram_t ram; uint16_t last_ram, first_open; } sizes[] = {
        { ACE_RAM_3K,  0x0000u, 0x4000u },
        { ACE_RAM_19K, 0x7FFFu, 0x8000u },
        { ACE_RAM_51K, 0xFFFFu, 0x0000u },
    };
    for (size_t i = 0; i < 3; i++) {
        CHECK(boot(sizes[i].ram), "ace_init refused RAM size %zu", i);
        if (sizes[i].last_ram) {
            poke(0x4000, 0x5A);
            poke(sizes[i].last_ram, 0xA5);
            CHECK(peek(0x4000) == 0x5A && peek(sizes[i].last_ram) == 0xA5,
                  "size %zu: expansion RAM at $4000 or $%04X", i, sizes[i].last_ram);
        }
        if (sizes[i].first_open) {
            poke(sizes[i].first_open, 0xFC);
            CHECK(peek(sizes[i].first_open) == m.cfg.open_bus,
                  "size %zu: unpopulated $%04X read $%02X", i, sizes[i].first_open,
                  peek(sizes[i].first_open));
            CHECK(peek(0xFFFF) == m.cfg.open_bus, "size %zu: $FFFF is populated", i);
        }
    }

    /* ---- The even port, decoded on A0 alone (§6.5). */
    CHECK(boot(ACE_RAM_19K), "reboot");
    ace_key_set(&m, 1, 0, true);           /* A, half-row $FD, D0         */
    ace_key_set(&m, 6, 4, true);           /* H, half-row $BF, D4         */
    CHECK(in_port(0xFDFE) == 0xFE, "A: $FDFE read $%02X", in_port(0xFDFE));
    CHECK(in_port(0xFD00) == 0xFE, "$FD00 is even and reads the keys: $%02X", in_port(0xFD00));
    CHECK(in_port(0xFD7C) == 0xFE, "$FD7C is even and reads the keys: $%02X", in_port(0xFD7C));
    CHECK(in_port(0xBFFE) == 0xEF, "H: $BFFE read $%02X", in_port(0xBFFE));
    CHECK(in_port(0xBDFE) == 0xEE, "two half-rows AND together: $%02X", in_port(0xBDFE));
    CHECK(in_port(0x00FE) == 0xEE, "all half-rows: $%02X", in_port(0x00FE));
    CHECK(in_port(0xFEFE) == 0xFF, "an unpressed half-row: $%02X", in_port(0xFEFE));
    /* Odd ports have nothing on the stock machine. */
    CHECK(in_port(0xFDFF) == m.cfg.open_bus, "odd port $FDFF read $%02X", in_port(0xFDFF));
    ace_key_set(&m, 1, 0, false);
    CHECK(in_port(0x00FE) == 0xEF, "a released key still reads: $%02X", in_port(0x00FE));
    ace_key_set(&m, 1, 7, true);           /* out of range: ignored       */
    CHECK(in_port(0x00FE) == 0xEF, "a column past D4 reached the port");

    /* The tape input is D5, active low; idle reads 1 (§16). */
    m.tape_in = false;
    CHECK(in_port(0xFFFE) == 0xDF, "tape low: $%02X", in_port(0xFFFE));
    m.tape_in = true;

    /* The speaker: IN from an even port drives it low, OUT high, and an
     * odd port moves nothing (§2.3, §8). */
    uint32_t e = m.beeper.edges;
    out_port(0x00FE, 0x00);
    CHECK(m.speaker && m.beeper.edges == e + 1, "OUT $FE did not raise the speaker");
    out_port(0x01FC, 0xFF);
    CHECK(m.speaker && m.beeper.edges == e + 1, "a second OUT made an edge");
    in_port(0xFFFF);
    out_port(0x00FF, 0);
    CHECK(m.speaker && m.beeper.edges == e + 1, "an odd port moved the speaker");
    in_port(0xFEFE);
    CHECK(!m.speaker && m.beeper.edges == e + 2, "IN $FE did not lower the speaker");

    /* An interrupt acknowledge asserts /IORQ with A0 low, but not /RD,
     * which the port's read strobe needs (schematic, §2.3): taking INT in
     * any mode leaves the speaker where it was. Only the acceptance is
     * stepped, not the handler, whose key scan is all INs. */
    for (uint8_t im = 0; im <= 2; im++) {
        out_port(0x00FE, 0);
        uint32_t e0 = m.beeper.edges;
        m.cpu.sp = 0x3F00;
        m.cpu.i = 0x3C;
        m.cpu.pc = 0x4000;
        m.cpu.im = im;
        m.cpu.iff1 = m.cpu.iff2 = 1;
        z80_set_int(&m.cpu, true);
        z80_step(&m.cpu);
        z80_set_int(&m.cpu, false);
        CHECK(m.cpu.iff1 == 0, "IM %u: INT was not taken", im);
        CHECK(m.speaker && m.beeper.edges == e0, "IM %u: the acknowledge moved the speaker", im);
    }

    /* ---- ace_copy: the copy has its own memory, and runs. */
    static ace_t c;
    poke(0x4000, 0x11);
    ace_copy(&c, &m);
    poke(0x4000, 0x22);
    CHECK(ace_peek(&c, 0x4000) == 0x11, "the copy shares the original's RAM");
    CHECK(c.page[0x40].read == &c.xram[0] && c.page[0x20].write == &c.vram[0],
          "the copy's page table points outside it");
    CHECK(c.page[0x00].read == ace_rom, "the copy lost the ROM");
    CHECK(c.cpu.bus.ctx == &c && c.cpu.bus.page == c.page, "the copy's CPU is on the original's bus");

    /* ---- ace_init refuses what it cannot run, and leaves m alone. */
    ace_config_t bad;
    ace_config_default(&bad);
    CHECK(!ace_init(&m, &bad), "accepted no ROM");
    bad.rom = ace_rom;
    bad.int_line = bad.active_line;
    CHECK(!ace_init(&m, &bad), "accepted INT at the snapshot point");
    bad.int_line = 248;
    bad.int_t = bad.field_lines * bad.line_t;
    CHECK(!ace_init(&m, &bad), "accepted INT longer than the field");
    /* A shape whose arithmetic wraps in 32 bits (review of PR #2). */
    bad.field_lines = 2;
    bad.line_t = UINT32_MAX;
    bad.active_line = 0;
    bad.int_line = 1;
    bad.int_t = 1;
    CHECK(!ace_init(&m, &bad), "accepted a field that wraps");
    bad.line_t = (uint32_t)(INT32_MAX / 2 + 1);
    CHECK(!ace_init(&m, &bad), "accepted a field longer than the signed budget");
    CHECK(ace_peek(&m, 0x4000) == 0x22, "a refused ace_init changed the machine");

    TEST_DONE();
}
