/* bench.c — M2's workloads (design.md §15.2 M2). See bench.h. */
#include "bench.h"

#include <string.h>

/* ---- The flat bus ------------------------------------------------------ */

#define BDOS_STUB 0xFE00u   /* also the top of the TPA: ZEX puts SP here */
#define TRAP_PORT 0x00u

static void put(bench_t *b, char ch) {
    if (b->out_len + 1 < sizeof b->out)
        b->out[b->out_len++] = ch;
    b->out[b->out_len] = 0;
}

static uint8_t mem_read(void *ctx, uint16_t a) {
    return ((bench_t *)ctx)->mem[a];
}

static void mem_write(void *ctx, uint16_t a, uint8_t v) {
    bench_t *b = ctx;
    b->mem[a] = v;
    b->slow_writes++;
}

static uint8_t io_read(void *ctx, uint16_t port) {
    (void)ctx;
    (void)port;
    return 0xFF;
}

/* The BDOS, as test_z80_zex has it: C is the function, E the character,
 * DE the '$'-terminated string. */
static void io_write(void *ctx, uint16_t port, uint8_t v) {
    bench_t *b = ctx;
    z80_t *c = &b->cpu;
    (void)v;
    if ((port & 0xFFu) != TRAP_PORT)
        return;
    if (c->bc.b.l == 2) {
        put(b, (char)c->de.b.l);
    } else if (c->bc.b.l == 9) {
        for (uint16_t a = c->de.w, n = 0; b->mem[a] != '$' && n < 1024; a++, n++)
            put(b, (char)b->mem[a]);
    }
}

static void machine_clear(bench_t *b) {
    memset(b->mem, 0, sizeof b->mem);
    for (unsigned p = 0; p < Z80_PAGE_COUNT; p++)
        b->pages[p] = (page_t){ b->mem + p * 256u, b->mem + p * 256u };
    b->out_len = 0;
    b->out[0] = 0;
    b->slow_writes = 0;
    b->cpu.bus = (z80_bus_t){ b->pages, b, mem_read, mem_write, io_read, io_write };
    z80_reset(&b->cpu);
    b->cpu.t = 0;
    b->cpu.insns = 0;
    b->cpu.ed_holes = 0;
}

/* ---- forth: a minimal assembler ---------------------------------------- */

typedef struct {
    uint8_t *mem;
    uint16_t at;
    uint16_t next;      /* NEXT's address, for JP NEXT */
} asm_t;

static void b8(asm_t *a, uint8_t v) { a->mem[a->at++] = v; }

static void b16(asm_t *a, uint16_t v) {
    b8(a, (uint8_t)v);
    b8(a, (uint8_t)(v >> 8));
}

static void bytes(asm_t *a, const uint8_t *p, size_t n) {
    while (n--)
        b8(a, *p++);
}

#define CODE(a, ...)                                                       \
    do {                                                                   \
        static const uint8_t code_[] = { __VA_ARGS__ };                    \
        bytes((a), code_, sizeof code_);                                   \
    } while (0)

static void jp_next(asm_t *a) {
    b8(a, 0xC3);                        /* JP NEXT */
    b16(a, a->next);
}

/* A primitive's header: its code field holds the address of the machine
 * code that follows it. Returns the code field address (CFA). */
static uint16_t prim(asm_t *a) {
    uint16_t cfa = a->at;
    b16(a, (uint16_t)(cfa + 2));
    return cfa;
}

/* An indirect-threaded Forth, in the classic Z80 register use: BC is the
 * instruction pointer IP, SP the data stack, IX the return stack (growing
 * down), and HL, DE and A scratch. Every primitive ends in JP NEXT, which
 * leaves DE at CFA + 1, so a colon word's body is at DE + 1.
 *
 * It is a stand-in, written to the shape of a Forth inner interpreter, not
 * a copy of the Ace ROM's: M3 runs the real one (design.md §3.2, §13.3). */
void bench_forth_load(bench_t *b) {
    machine_clear(b);
    asm_t a = { b->mem, 0x0100u, 0x0100u };

    /* NEXT: W = (IP), IP += 2, jump to (W). 62 T. */
    CODE(&a,
         0x0A,                  /* LD A,(BC)                    */
         0x6F,                  /* LD L,A                       */
         0x03,                  /* INC BC                       */
         0x0A,                  /* LD A,(BC)                    */
         0x67,                  /* LD H,A                       */
         0x03,                  /* INC BC        HL = CFA       */
         0x5E,                  /* LD E,(HL)                    */
         0x23,                  /* INC HL                       */
         0x56,                  /* LD D,(HL)                    */
         0xEB,                  /* EX DE,HL      DE = CFA + 1   */
         0xE9);                 /* JP (HL)                      */

    /* DOCOL: push IP on the return stack; IP = the body at DE + 1. */
    uint16_t docol = a.at;
    CODE(&a,
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x70, 0x00,      /* LD (IX+0),B                  */
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x71, 0x00,      /* LD (IX+0),C                  */
         0x13,                  /* INC DE                       */
         0x42,                  /* LD B,D                       */
         0x4B);                 /* LD C,E                       */
    jp_next(&a);

    uint16_t exit_ = prim(&a);
    CODE(&a,
         0xDD, 0x4E, 0x00,      /* LD C,(IX+0)                  */
         0xDD, 0x46, 0x01,      /* LD B,(IX+1)                  */
         0xDD, 0x23,            /* INC IX                       */
         0xDD, 0x23);           /* INC IX                       */
    jp_next(&a);

    uint16_t lit = prim(&a);    /* ( -- n ), n inline           */
    CODE(&a,
         0x0A,                  /* LD A,(BC)                    */
         0x6F,                  /* LD L,A                       */
         0x03,                  /* INC BC                       */
         0x0A,                  /* LD A,(BC)                    */
         0x67,                  /* LD H,A                       */
         0x03,                  /* INC BC                       */
         0xE5);                 /* PUSH HL                      */
    jp_next(&a);

    uint16_t plus = prim(&a);   /* ( a b -- a+b )               */
    CODE(&a,
         0xE1,                  /* POP HL                       */
         0xD1,                  /* POP DE                       */
         0x19,                  /* ADD HL,DE                    */
         0xE5);                 /* PUSH HL                      */
    jp_next(&a);

    uint16_t dup = prim(&a);    /* ( a -- a a )                 */
    CODE(&a,
         0xE1,                  /* POP HL                       */
         0xE5,                  /* PUSH HL                      */
         0xE5);                 /* PUSH HL                      */
    jp_next(&a);

    uint16_t store = prim(&a);  /* ( n addr -- )                */
    CODE(&a,
         0xE1,                  /* POP HL        addr           */
         0xD1,                  /* POP DE        n              */
         0x73,                  /* LD (HL),E                    */
         0x23,                  /* INC HL                       */
         0x72);                 /* LD (HL),D                    */
    jp_next(&a);

    /* (DO) ( limit index -- ): the return stack gets index at IX+0 and
     * limit at IX+2, low bytes first. */
    uint16_t xdo = prim(&a);
    CODE(&a,
         0xD1,                  /* POP DE        index          */
         0xE1,                  /* POP HL        limit          */
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x74, 0x00,      /* LD (IX+0),H                  */
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x75, 0x00,      /* LD (IX+0),L                  */
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x72, 0x00,      /* LD (IX+0),D                  */
         0xDD, 0x2B,            /* DEC IX                       */
         0xDD, 0x73, 0x00);     /* LD (IX+0),E                  */
    jp_next(&a);

    uint16_t i_ = prim(&a);     /* ( -- index )                 */
    CODE(&a,
         0xDD, 0x6E, 0x00,      /* LD L,(IX+0)                  */
         0xDD, 0x66, 0x01,      /* LD H,(IX+1)                  */
         0xE5);                 /* PUSH HL                      */
    jp_next(&a);

    /* (LOOP), branch address inline: index += 1; while it is not the
     * limit, IP = the branch address, else drop the loop and skip it. */
    uint16_t xloop = prim(&a);
    CODE(&a,
         0xDD, 0x6E, 0x00,      /* LD L,(IX+0)                  */
         0xDD, 0x66, 0x01,      /* LD H,(IX+1)                  */
         0x23,                  /* INC HL                       */
         0xDD, 0x75, 0x00,      /* LD (IX+0),L                  */
         0xDD, 0x74, 0x01,      /* LD (IX+1),H                  */
         0xDD, 0x5E, 0x02,      /* LD E,(IX+2)                  */
         0xDD, 0x56, 0x03,      /* LD D,(IX+3)                  */
         0xB7,                  /* OR A                         */
         0xED, 0x52,            /* SBC HL,DE                    */
         0x28, 0x09,            /* JR Z,done   (over the 9 below) */
         0x0A,                  /* LD A,(BC)                    */
         0x6F,                  /* LD L,A                       */
         0x03,                  /* INC BC                       */
         0x0A,                  /* LD A,(BC)                    */
         0x47,                  /* LD B,A                       */
         0x4D);                 /* LD C,L                       */
    jp_next(&a);
    CODE(&a,                    /* done:                        */
         0x03,                  /* INC BC                       */
         0x03,                  /* INC BC                       */
         0x11, 0x04, 0x00,      /* LD DE,4                      */
         0xDD, 0x19);           /* ADD IX,DE                    */
    jp_next(&a);

    uint16_t branch = prim(&a); /* IP = the address inline      */
    CODE(&a,
         0x0A,                  /* LD A,(BC)                    */
         0x6F,                  /* LD L,A                       */
         0x03,                  /* INC BC                       */
         0x0A,                  /* LD A,(BC)                    */
         0x47,                  /* LD B,A                       */
         0x4D);                 /* LD C,L                       */
    jp_next(&a);

    /* : DOUBLE  DUP + ;  a colon word, for DOCOL and EXIT. */
    uint16_t twice = a.at;
    b16(&a, docol);
    b16(&a, dup);
    b16(&a, plus);
    b16(&a, exit_);

    /* The program, run for ever:
     *   BEGIN  0  1000 0 DO  I DOUBLE +  LOOP  RESULT !  AGAIN */
    uint16_t top = a.at;
    b16(&a, lit);   b16(&a, 0);
    b16(&a, lit);   b16(&a, 1000);
    b16(&a, lit);   b16(&a, 0);
    b16(&a, xdo);
    uint16_t body = a.at;
    b16(&a, i_);
    b16(&a, twice);
    b16(&a, plus);
    b16(&a, xloop); b16(&a, body);
    b16(&a, lit);   b16(&a, BENCH_FORTH_RESULT);
    b16(&a, store);
    b16(&a, branch); b16(&a, top);

    z80_t *c = &b->cpu;
    c->bc.w = top;
    c->sp = BENCH_FORTH_SP;
    c->ix.w = BENCH_FORTH_RP;
    c->pc = a.next;
}

/* ---- zexdoc ------------------------------------------------------------ */

bool bench_zex_load(bench_t *b, const uint8_t *com, size_t len) {
    machine_clear(b);
    if (len == 0 || len > BDOS_STUB - 0x0100u)
        return false;
    memcpy(b->mem + 0x0100, com, len);

    b->mem[0x0000] = 0x76;                       /* warm boot: HALT       */
    b->mem[0x0005] = 0xC3;                       /* JP BDOS_STUB          */
    b->mem[0x0006] = BDOS_STUB & 0xFF;
    b->mem[0x0007] = BDOS_STUB >> 8;
    b->mem[BDOS_STUB + 0] = 0xD3;                /* OUT (TRAP_PORT),A     */
    b->mem[BDOS_STUB + 1] = TRAP_PORT;
    b->mem[BDOS_STUB + 2] = 0xC9;                /* RET                   */

    b->cpu.pc = 0x0100;
    b->cpu.sp = BDOS_STUB;
    return true;
}

/* ---- Running ----------------------------------------------------------- */

uint64_t bench_run(bench_t *b, uint64_t t_states) {
    uint64_t done = 0;
    while (done < t_states && !b->cpu.halted)
        done += z80_run(&b->cpu, BENCH_SLICE_T);
    return done;
}
