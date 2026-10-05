/* test_z80_behaviour.c — what FUSE's tests cannot see (design.md §5.1,
 * §5.3, §5.4): interrupts, HALT, the run loop's contract, and Q.
 *
 * FUSE never raises an interrupt, and starts every test with Q at zero.
 * The interrupt rules are written against the Z80 user manual, and Q
 * against Patrik Rak's measurements of NMOS Z80s. Each rule is asserted
 * beside a control that runs the same code under the other condition and
 * must come out differently (§13.1).
 */
#include <stdint.h>
#include <string.h>

#include "test_util.h"
#include "z80.h"

static uint8_t mem[65536];
static page_t  pages[Z80_PAGE_COUNT];
static z80_t   cpu;

static uint8_t unused_read(void *ctx, uint16_t a) { (void)ctx; (void)a; return 0xFF; }
static void unused_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; (void)a; (void)v; }

/* A flat 64 KiB machine with prog at $0000, SP at $8000 and INT low. */
static z80_t *boot(const uint8_t *prog, size_t n) {
    memset(mem, 0, sizeof mem);
    memcpy(mem, prog, n);
    for (unsigned p = 0; p < Z80_PAGE_COUNT; p++)
        pages[p] = (page_t){ mem + p * 256u, mem + p * 256u };
    memset(&cpu, 0, sizeof cpu);
    cpu.bus = (z80_bus_t){ pages, NULL, unused_read, unused_write, unused_read, unused_write, NULL, NULL };
    z80_reset(&cpu);
    cpu.sp = 0x8000;
    return &cpu;
}

static uint16_t stack_top(const z80_t *c) {
    return (uint16_t)(mem[c->sp] | (mem[(uint16_t)(c->sp + 1)] << 8));
}

#define IM1 0xED, 0x56
#define IM2 0xED, 0x5E
#define EI  0xFB
#define DI  0xF3
#define NOP 0x00
#define HALT 0x76

static int test_ei_delay(void) {
    /* EI holds an interrupt off for one more instruction: the HALT after
     * it runs, and the interrupt wakes it. */
    static const uint8_t prog[] = { IM1, EI, HALT };
    z80_t *c = boot(prog, sizeof prog);
    CHECK(z80_step(c) == 8, "IM 1");
    z80_set_int(c, true);
    CHECK(z80_step(c) == 4 && c->iff1 && c->iff2, "EI");
    CHECK(z80_step(c) == 4 && c->halted && c->pc == 0x0003,
          "the instruction after EI did not run first (pc %04x)", c->pc);
    CHECK(z80_step(c) == 13, "IM 1 acceptance is 13 T");
    CHECK(c->pc == 0x0038 && !c->halted && !c->iff1 && !c->iff2, "pc %04x", c->pc);
    CHECK(stack_top(c) == 0x0004, "pushed %04x, not the address after HALT", stack_top(c));

    /* Control: two instructions after EI, INT is taken at once. */
    static const uint8_t ctrl[] = { IM1, EI, NOP, NOP, NOP };
    c = boot(ctrl, sizeof ctrl);
    z80_step(c);
    z80_step(c);
    z80_step(c);
    z80_set_int(c, true);
    CHECK(z80_step(c) == 13 && stack_top(c) == 0x0004, "control: not taken at once");

    /* EI EI: each holds it off again; the NOP after the second runs. */
    static const uint8_t chain[] = { IM1, EI, EI, NOP, NOP };
    c = boot(chain, sizeof chain);
    z80_step(c);
    z80_set_int(c, true);
    z80_step(c);
    z80_step(c);
    CHECK(z80_step(c) == 4 && c->pc == 0x0005, "the NOP after EI EI did not run");
    CHECK(z80_step(c) == 13 && stack_top(c) == 0x0005, "EI EI");
    return 0;
}

static int test_halt(void) {
    /* With interrupts off, HALT repeats as a 4 T NOP that counts in R. */
    static const uint8_t prog[] = { DI, HALT };
    z80_t *c = boot(prog, sizeof prog);
    z80_set_int(c, true);
    z80_step(c);
    uint8_t r0 = z80_r(c);
    uint32_t t = 0;
    for (int k = 0; k < 200; k++)
        t += z80_step(c);
    CHECK(t == 800, "200 halted steps took %u T", (unsigned)t);
    CHECK(c->halted && c->pc == 0x0001, "pc %04x", c->pc);
    CHECK(z80_r(c) == ((r0 + 200) & 0x7F), "R %02x, from %02x", z80_r(c), r0);

    /* R's bit 7 is kept as LD R,A left it. */
    static const uint8_t r7[] = { 0x3E, 0xFF, 0xED, 0x4F, HALT };   /* LD A,$FF; LD R,A */
    c = boot(r7, sizeof r7);
    for (int k = 0; k < 300; k++)
        z80_step(c);
    CHECK(z80_r(c) & 0x80, "R bit 7 lost");
    return 0;
}

static int test_modes(void) {
    /* IM 2: the vector is read from I:bus byte. */
    static const uint8_t prog[] = { IM2, 0x3E, 0x40, 0xED, 0x47, EI, NOP, NOP };
    z80_t *c = boot(prog, sizeof prog);
    mem[0x40FE] = 0x34;
    mem[0x40FF] = 0x12;
    c->int_data = 0xFE;
    for (int k = 0; k < 5; k++)
        z80_step(c);
    z80_set_int(c, true);
    CHECK(z80_step(c) == 19, "IM 2 acceptance is 19 T");
    CHECK(c->pc == 0x1234 && c->wz.w == 0x1234, "IM 2 went to %04x", c->pc);

    /* IM 0: the bus byte is an RST. $FF, the Ace's (§16), is RST $38. */
    static const uint8_t im0[] = { 0xED, 0x46, EI, NOP, NOP };
    c = boot(im0, sizeof im0);
    for (int k = 0; k < 3; k++)
        z80_step(c);
    z80_set_int(c, true);
    CHECK(z80_step(c) == 13 && c->pc == 0x0038, "IM 0 with $FF went to %04x", c->pc);
    c = boot(im0, sizeof im0);
    c->int_data = 0xD7;                                          /* RST $10 */
    for (int k = 0; k < 3; k++)
        z80_step(c);
    z80_set_int(c, true);
    CHECK(z80_step(c) == 13 && c->pc == 0x0010, "IM 0 with $D7 went to %04x", c->pc);
    return 0;
}

/* A level, not an edge (§5.3): INT released before interrupts are
 * enabled is lost, and INT still held then is taken. */
static int test_level(void) {
    static const uint8_t prog[] = {
        IM1, DI,
        0x06, 0x10,                 /* LD B,16                       */
        0x10, 0xFE,                 /* DJNZ $: 15 x 13 + 8 = 203 T   */
        EI, NOP,
        0x18, 0xFE,                 /* $0009: JR $                   */
    };
    static const uint8_t isr[] = {
        0x3E, 0x55, 0x32, 0x00, 0x90, HALT,     /* LD A,$55; LD ($9000),A; HALT */
    };

    for (int held = 0; held < 2; held++) {
        z80_t *c = boot(prog, sizeof prog);
        memcpy(mem + 0x38, isr, sizeof isr);
        z80_set_int(c, true);
        z80_run(c, 100);            /* still in the DI loop */
        if (!held)
            z80_set_int(c, false);
        z80_run(c, 2000);
        if (held) {
            CHECK(mem[0x9000] == 0x55, "held INT was not taken");
            CHECK(stack_top(c) == 0x0009, "taken at %04x, not after EI's NOP",
                  stack_top(c));
        } else {
            CHECK(mem[0x9000] == 0, "a pulse released under DI was taken");
        }
    }
    return 0;
}

static int test_ld_a_i(void) {
    /* An interrupt taken right after LD A,I reads P/V as 0 (NMOS). */
    static const uint8_t prog[] = { IM1, EI, NOP, 0xED, 0x57, NOP, NOP };
    for (int gap = 0; gap < 2; gap++) {
        z80_t *c = boot(prog, sizeof prog);
        for (int k = 0; k < 4 + gap; k++)
            z80_step(c);                /* to just after LD A,I, or its NOP */
        CHECK(c->af.b.l & Z80_FPV, "LD A,I did not copy IFF2");
        z80_set_int(c, true);
        CHECK(z80_step(c) == 13, "not accepted");
        if (gap)
            CHECK(c->af.b.l & Z80_FPV, "control: P/V cleared after a NOP");
        else
            CHECK(!(c->af.b.l & Z80_FPV), "P/V survived acceptance after LD A,I");
    }
    return 0;
}

static int test_nmi(void) {
    /* NMI ignores DI's IFF1, keeps IFF2, and RETN puts IFF1 back. */
    static const uint8_t prog[] = { EI, NOP, NOP, NOP };
    z80_t *c = boot(prog, sizeof prog);
    mem[0x66] = 0xED;
    mem[0x67] = 0x45;                                            /* RETN */
    z80_step(c);
    z80_step(c);
    z80_nmi(c);
    CHECK(z80_step(c) == 11, "NMI acceptance is 11 T");
    CHECK(c->pc == 0x0066 && !c->iff1 && c->iff2, "pc %04x iff %d %d", c->pc, c->iff1,
          c->iff2);
    CHECK(z80_step(c) == 14 && c->pc == 0x0002 && c->iff1, "RETN");

    /* RETI copies IFF2 as well. */
    static const uint8_t reti[] = { DI, 0xED, 0x4D };
    c = boot(reti, sizeof reti);
    mem[0x7FFE] = 0x00;
    mem[0x7FFF] = 0x10;
    c->sp = 0x7FFE;
    z80_step(c);
    c->iff2 = 1;
    z80_step(c);
    CHECK(c->iff1 == 1 && c->pc == 0x1000, "RETI");
    return 0;
}

static int test_run_contract(void) {
    /* Whole instructions, never fewer T than asked for (§4.2). */
    static const uint8_t prog[] = { 0x01, 0x34, 0x12, NOP };     /* LD BC,nn: 10 T */
    z80_t *c = boot(prog, sizeof prog);
    CHECK(z80_run(c, 0) == 0 && c->pc == 0, "run(0) ran something");
    CHECK(z80_run(c, 1) == 10 && c->pc == 3, "run(1) is one whole LD BC,nn");
    CHECK(z80_run(c, 5) == 8, "run(5) over NOPs is two");
    CHECK(c->insns == 3, "counted %u instructions, not 3", (unsigned)c->insns);

    /* ED holes are counted NOPs of 8 T. */
    static const uint8_t holes[] = { 0xED, 0x00, 0xED, 0x77, 0xED, 0xFF };
    c = boot(holes, sizeof holes);
    CHECK(z80_run(c, 24) == 24 && c->ed_holes == 3, "%u holes", (unsigned)c->ed_holes);

    /* A run of prefixes is one 4 T step each, and no interrupt comes
     * between a prefix and its opcode. */
    static const uint8_t chain[] = { IM1, EI, 0xDD, 0xDD, NOP, NOP };
    c = boot(chain, sizeof chain);
    z80_step(c);
    z80_set_int(c, true);
    CHECK(z80_step(c) == 4, "EI");
    CHECK(z80_step(c) == 4, "lone DD");
    CHECK(z80_step(c) == 8 && c->pc == 0x0006, "DD NOP");
    CHECK(z80_step(c) == 13 && stack_top(c) == 0x0006, "INT after the chain");
    /* Each step counts once: IM 1, EI, the lone DD, DD NOP, the INT. */
    CHECK(c->insns == 5, "counted %u steps, not 5", (unsigned)c->insns);

    /* Memory full of prefixes still returns from a run. */
    c = boot(prog, 0);
    memset(mem, 0xFD, sizeof mem);
    z80_set_int(c, true);
    uint32_t t = z80_run(c, 100000);
    CHECK(t >= 100000 && t < 100004, "%u T", (unsigned)t);
    return 0;
}

/* The registers and flags stepping would leave; not the counters, the
 * option or the bus. */
static bool same_state(const z80_t *a, const z80_t *b) {
    z80_t x = *a, y = *b;
    x.insns = y.insns = x.halts = y.halts = x.run_end = y.run_end = 0;
    x.halt_skip = y.halt_skip = false;
    memset(&x.bus, 0, sizeof x.bus);
    memset(&y.bus, 0, sizeof y.bus);
    return memcmp(&x, &y, sizeof x) == 0;
}

/* HALT fast-forward (§5.3): a run that skips the repeats ends where one
 * that steps them does, every instruction counted once in insns or in
 * halts. Run lengths not a multiple of 4 check the overshoot. */
static int test_halt_skip(void) {
    static z80_t step;
    static const uint8_t prog[] = { 0xAF, 0xFE, 0x28, DI, HALT };   /* q and F set first */
    static const uint32_t lens[] = { 1, 3, 4, 5, 64, 1001, 69888 };
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        for (int k = 0; k < 2; k++) {
            z80_t *c = boot(prog, sizeof prog);
            c->halt_skip = k;
            uint32_t t = z80_run(c, 30);
            /* A HALT that has run has cleared both; a restored state
             * need not have, and the repeats clear them as well. */
            c->q = 0x28;
            c->ld_a_ir = true;
            t += z80_run(c, lens[i]);
            if (!k) step = *c;
            else {
                CHECK(t == (uint32_t)(step.t), "run of %u: %u T, stepping %u",
                      (unsigned)lens[i], (unsigned)t, (unsigned)step.t);
                CHECK(same_state(c, &step), "run of %u: state differs from stepping",
                      (unsigned)lens[i]);
                CHECK(c->insns + c->halts == step.insns, "run of %u: %u + %u, stepping %u",
                      (unsigned)lens[i], (unsigned)c->insns, (unsigned)c->halts,
                      (unsigned)step.insns);
                CHECK(step.halts == 0, "stepping skipped %u", (unsigned)step.halts);
                CHECK(lens[i] < 8 || c->halts > 0, "run of %u skipped nothing",
                      (unsigned)lens[i]);
            }
        }
    }

    /* An interrupt the CPU would take is taken, not skipped past: with
     * INT held and IFF1 set the run leaves the HALT at once. Control:
     * with INT low the same run stays halted to the end, running the
     * HALT once and skipping the other 249. */
    static const uint8_t ei[] = { IM1, EI, HALT };
    for (int held = 0; held < 2; held++) {
        z80_t *c = boot(ei, sizeof ei);
        c->halt_skip = true;
        z80_run(c, 16);                                 /* IM 1, EI, HALT */
        CHECK(c->halted, "not halted after the HALT");
        z80_set_int(c, held);
        uint32_t t = z80_run(c, 1000);
        if (held)
            CHECK(!c->halted && c->pc != 0x0003 && c->halts == 0,
                  "INT held: pc %04x, %u skipped", c->pc, (unsigned)c->halts);
        else
            CHECK(c->halted && c->pc == 0x0003 && t == 1000 && c->halts == 249,
                  "INT low: pc %04x, %u T, %u skipped", c->pc, (unsigned)t, (unsigned)c->halts);
    }

    /* EI; HALT with INT held: the HALT runs once, and the interrupt after
     * it, at the end of the HALT's one step, wakes it. */
    z80_t *c = boot(ei, sizeof ei);
    c->halt_skip = true;
    z80_step(c);
    z80_set_int(c, true);
    z80_run(c, 8);                                      /* EI, then HALT */
    CHECK(c->halted && c->int_blocked == false, "after EI; HALT");
    z80_run(c, 1);
    CHECK(c->pc == 0x0038 && c->halts == 0, "pc %04x", c->pc);

    /* The same in one long run: the HALT, held off by EI, must not skip
     * past the interrupt that is due as soon as it has run. */
    c = boot(ei, sizeof ei);
    c->halt_skip = true;
    z80_step(c);
    z80_set_int(c, true);
    uint32_t t = z80_run(c, 1000);
    CHECK(c->halts == 0 && t < 1000 + 4, "%u skipped", (unsigned)c->halts);
    CHECK(c->pc != 0x0003 && !c->halted, "still halted at %04x", c->pc);
    return 0;
}

/* SCF and CCF take X and Y from (Q ^ F) | A, where Q is F if the
 * previous instruction wrote the flags and 0 if it did not. CP $28 leaves
 * X and Y set in F and clear in A, so they survive SCF only when an
 * instruction that leaves the flags alone comes between. */
static int test_q(void) {
    for (int op = 0; op < 2; op++) {
        uint8_t scf_ccf = op ? 0x3F : 0x37;
        for (int gap = 0; gap < 2; gap++) {
            const uint8_t prog[] = { 0xAF, 0xFE, 0x28, NOP, scf_ccf };  /* XOR A; CP $28 */
            z80_t *c = boot(prog, sizeof prog);
            if (!gap) {
                mem[3] = scf_ccf;                       /* SCF/CCF straight after CP */
                mem[4] = NOP;
            }
            for (int k = 0; k < 3 + gap; k++)
                z80_step(c);
            uint8_t xy = c->af.b.l & (Z80_FX | Z80_FY);
            if (gap)
                CHECK(xy == 0x28, "%s after a NOP: X/Y %02x, not from F", op ? "CCF" : "SCF", xy);
            else
                CHECK(xy == 0x00, "%s after CP: X/Y %02x, not from A", op ? "CCF" : "SCF", xy);
        }
    }
    return 0;
}

int main(void) {
    if (test_ei_delay() || test_halt() || test_modes() || test_level() || test_ld_a_i() ||
        test_nmi() || test_run_contract() || test_halt_skip() || test_q())
        return 1;
    TEST_DONE();
}
