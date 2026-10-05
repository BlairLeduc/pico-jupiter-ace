/* tape.c — tape phase 1: the block routines' trap (tape.h, design.md §10.3).
 *
 * The addresses are the stock ROM's, read off its routines 2026-10-04:
 *
 *   $1820 save: PUSH IY, IY = HL, push $1892 (the exit), leader, sync,
 *         then C, the DE bytes and their XOR, each followed at $1872 by
 *         DEC DE, INC IY and a BREAK check. No CALLs.
 *   $18A7 load: DI, PUSH IY, IY = HL, push $1892, H = C, EX AF,AF' (the
 *         caller's carry chooses load or verify), leader, sync, then
 *         bytes by CALL $18FC, which ends at $190C: H ^= L, SCF, RET.
 *         The first byte is the flag (back at $18DA: CCF, RET NZ on a
 *         mismatch); each later one comes back to $18F3, is stored or
 *         compared, and the byte after the last is the checksum, tested
 *         at $18F8 by LD A,H : CP 1.
 *   $1892 the exit: POP IY, EX AF,AF', a delay, OUT ($FE),0, the BREAK
 *         check (IN from $7FFE, JP NC $04F0), EI, EX AF,AF', RET.
 */

#include "tape.h"

#include <string.h>

#include "ace.h"
#include "ace_rom.h"

/* Where the trap hands the machine back to the ROM. */
#define BYTE_DONE_PC   0x190Cu   /* LD A,H : XOR L : LD H,A : SCF : RET  */
#define SAVE_TAIL_PC   0x1872u   /* DEC DE : INC IY : the BREAK check     */
#define EXIT_PC        0x1892u
#define AFTER_FLAG_RET 0x18DAu   /* CALL $18FC at $18D7, the flag byte    */
#define AFTER_BYTE_RET 0x18F3u   /* CALL $18FC at $18F0, every later byte */
#define IN_BYTE_RET    0x1903u   /* CALL $1911 at $1900, a bit            */
#define IN_EDGE_RET    0x1914u   /* CALL $1915 at $1911, an edge          */

/* The level the load routine last saw on the tape input, as C keeps it:
 * every bit the ROM writes ends low, so after a block played as it was
 * recorded C is $00. A tape played inverted would leave $FF. */
#define EDGE_LEVEL_C   0x00u

/* The save routine's OUT level: LD BC,$3B08 at $184C. */
#define SAVE_LEVEL_C   0x08u

uint8_t tape_checksum(const uint8_t *p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

/* Not const, so that they sit in SRAM beside the loop (EL §8.2). The
 * second adds the exit, a cue only while the deck runs (cassette.h): its
 * low byte is every page's $92, which is not worth a call otherwise. */
static uint8_t trap_lo[256] = {
    [TAPE_SAVE_PC & 0xFFu] = 1,
    [TAPE_LOAD_PC & 0xFFu] = 1,
};
static uint8_t trap_lo_exit[256] = {
    [TAPE_SAVE_PC & 0xFFu] = 1,
    [TAPE_LOAD_PC & 0xFFu] = 1,
    [EXIT_PC & 0xFFu] = 1,
};

void tape_hook(ace_t *m) {
    const cassette_t *c = &m->cas;
    bool on = m->tape.stock && (m->cfg.tape_traps || c->loaded);
    bool exit = c->playing || c->rec.on;
    m->cpu.bus.trap_lo = !on ? NULL : exit ? trap_lo_exit : trap_lo;
    m->cpu.bus.trap = on ? tape_trap : NULL;
}

void tape_init(ace_t *m) {
    tape_t *t = &m->tape;
    memset(t, 0, sizeof *t);
    t->stock = m->cfg.rom &&
               memcmp(m->cfg.rom + TAPE_ROM_FIRST, ace_rom + TAPE_ROM_FIRST,
                      TAPE_ROM_END - TAPE_ROM_FIRST) == 0;
    tape_hook(m);
}

static uint16_t peek16(const ace_t *m, uint16_t a) {
    return (uint16_t)(ace_peek(m, a) | (ace_peek(m, (uint16_t)(a + 1u)) << 8));
}

static void poke16(ace_t *m, uint16_t a, uint16_t v) {
    ace_poke(m, a, (uint8_t)v);
    ace_poke(m, (uint16_t)(a + 1u), (uint8_t)(v >> 8));
}

bool tape_trap(void *ctx) {
    ace_t *m = ctx;
    tape_t *t = &m->tape;
    if (t->op != TAPE_NONE) return true;

    z80_t *c = &m->cpu;
    if (c->pc == EXIT_PC) {
        cassette_cue_exit(m);
        return false;
    }
    tape_op_t op;
    if (c->pc == TAPE_SAVE_PC) op = TAPE_SAVE;
    else if (c->pc == TAPE_LOAD_PC) op = (c->af.b.l & Z80_FC) ? TAPE_LOAD : TAPE_VERIFY;
    else return false;

    /* Not served, by choice or by the port's decline: the ROM's routine
     * runs, and the deck follows its cue (cassette.h). */
    if (!m->cfg.tape_traps || t->pass) {
        t->pass = false;
        if (op == TAPE_SAVE) cassette_cue_save(m);
        else cassette_cue_load(m);
        return false;
    }

    memset(t, 0, offsetof(tape_t, stock));
    t->op   = op;
    t->addr = c->hl.w;
    t->len  = c->de.w;
    t->flag = c->bc.b.l;
    t->sp   = c->sp;
    t->ret  = peek16(m, c->sp);
    t->iy   = c->iy.w;
    return true;
}

const tape_t *ace_tape_pending(const ace_t *m) {
    return m->tape.op != TAPE_NONE ? &m->tape : NULL;
}

void ace_tape_decline(ace_t *m) {
    if (m->tape.op == TAPE_NONE) return;
    m->tape.op = TAPE_NONE;
    m->tape.pass = true;
    m->tape.declined++;
}

/* ---- load ------------------------------------------------------------ */

bool ace_tape_load_begin(ace_t *m, uint8_t flag) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_LOAD && t->op != TAPE_VERIFY) return false;
    t->begun = true;
    t->flag_ok = flag == t->flag;
    t->last = flag;
    t->at = 0;
    t->sum = 0;
    t->stopped = !t->flag_ok;
    return t->flag_ok;
}

bool ace_tape_load_wants(const ace_t *m) {
    const tape_t *t = &m->tape;
    return t->begun && !t->stopped;
}

void ace_tape_load_data(ace_t *m, const uint8_t *src, size_t n) {
    tape_t *t = &m->tape;
    for (size_t i = 0; i < n && ace_tape_load_wants(m); i++) {
        uint8_t b = src[i];
        if (t->at < t->len) {
            uint16_t a = (uint16_t)(t->addr + t->at);
            if (t->op == TAPE_VERIFY) {
                /* The ROM compares what is there, ROM or I/O included,
                 * and stops at the first difference ($18E7). */
                if (ace_peek(m, a) != b) {
                    t->stopped = true;
                    t->last = b;
                    break;
                }
            } else {
                ace_poke(m, a, b);
            }
            t->sum ^= b;
            t->at++;
        } else {
            /* The byte after the last is the checksum. */
            t->stopped = true;
            t->last = b;
        }
    }
}

/* The stack the load routine leaves below the caller's return address:
 * the IY it pushed, its exit, and the return addresses of the last calls
 * at each depth, which reading any byte leaves the same. */
static void load_stack(ace_t *m, uint16_t ret) {
    const tape_t *t = &m->tape;
    uint16_t s = t->sp;
    poke16(m, (uint16_t)(s - 2u), t->iy);
    poke16(m, (uint16_t)(s - 4u), EXIT_PC);
    poke16(m, (uint16_t)(s - 6u), ret);
    poke16(m, (uint16_t)(s - 8u), IN_BYTE_RET);
    poke16(m, (uint16_t)(s - 10u), IN_EDGE_RET);
}

void ace_tape_load_end(ace_t *m) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_LOAD && t->op != TAPE_VERIFY) return;
    z80_t *c = &m->cpu;

    /* Interrupts as the DI at $18A7 left them. */
    c->iff1 = c->iff2 = 0;
    c->int_blocked = false;
    c->q = 0;
    c->bc.b.h = 0;
    c->bc.b.l = EDGE_LEVEL_C;

    /* AF' is left as it is: the exit's EX AF,AF' and XOR A overwrite
     * whatever the routine kept there, and the IN after them sets it. */

    if (!t->flag_ok) {
        /* The flag byte just read: H = C ^ flag, then CCF, RET NZ. */
        load_stack(m, AFTER_FLAG_RET);
        c->hl.b.h = t->flag;
        c->hl.b.l = t->last;
        c->de.w = t->len;
        c->iy.w = t->addr;
        c->sp = (uint16_t)(t->sp - 6u);
        c->pc = BYTE_DONE_PC;
    } else if (t->stopped) {
        /* A verify's difference, or the checksum: the ROM tests it. */
        load_stack(m, AFTER_BYTE_RET);
        c->hl.b.h = t->sum;
        c->hl.b.l = t->last;
        c->de.w = (uint16_t)(t->len - t->at);
        c->iy.w = (uint16_t)(t->addr + t->at);
        c->sp = (uint16_t)(t->sp - 6u);
        c->pc = BYTE_DONE_PC;
    } else {
        /* The block ended before the ROM had what it asked for. On tape
         * the next leader would be read as bytes; here the read times
         * out, as with silence: sub $1915's INC B reaching zero, back to
         * $18F3 with carry clear (design.md §10.3). */
        load_stack(m, AFTER_BYTE_RET);
        c->hl.b.h = t->sum;
        c->hl.b.l = 0x01u;
        c->de.w = (uint16_t)(t->len - t->at);
        c->iy.w = (uint16_t)(t->addr + t->at);
        c->af.b.h = 0;
        c->af.b.l = Z80_FZ | Z80_FH;
        c->sp = (uint16_t)(t->sp - 4u);
        c->pc = AFTER_BYTE_RET;
    }
    c->wz.w = c->pc;
    t->op = TAPE_NONE;
    t->served++;
}

/* ---- save ------------------------------------------------------------ */

void ace_tape_save_end(ace_t *m) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_SAVE) return;
    z80_t *c = &m->cpu;
    uint16_t s = t->sp;

    poke16(m, (uint16_t)(s - 2u), t->iy);
    poke16(m, (uint16_t)(s - 4u), EXIT_PC);

    /* As the checksum has gone out: the DEC DE that makes DE $FFFF, and
     * the BREAK check after it, are the ROM's. The DI at $1835. */
    c->iff1 = c->iff2 = 0;
    c->int_blocked = false;
    c->q = 0;
    c->hl.w = 0;
    c->de.w = 0;
    c->bc.b.h = 0;
    c->bc.b.l = SAVE_LEVEL_C;
    /* IY as the routine has it; the exit's POP IY restores the
     * caller's. */
    c->iy.w = (uint16_t)(t->addr + t->len);
    c->sp = (uint16_t)(s - 4u);
    c->pc = c->wz.w = SAVE_TAIL_PC;
    t->op = TAPE_NONE;
    t->served++;
}
