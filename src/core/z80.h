/* z80.h — Zilog Z80 interpreter (design.md §5).
 *
 * Every opcode, the undocumented ones included (§5.1): IXH/IXL/IYH/IYL,
 * SLL, the DDCB/FDCB register copies, flags X and Y, MEMPTR and Q. T-states
 * are exact at instruction granularity; there is no per-T-state bus.
 *
 * The CPU knows nothing about the Ace. It reaches memory through a page
 * table of {read, write} pointers, falling back to the bus's functions for
 * a NULL pointer, and reaches I/O only through the bus (§5.2, §6.1).
 *
 * Time is z80_t.t, a T-state counter that wraps: compare by difference.
 */
#ifndef PICO_ACE_Z80_H
#define PICO_ACE_Z80_H

#include <stdbool.h>
#include <stdint.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "z80_pair_t assumes a little-endian host"
#endif

/* Flag bits of F. X and Y are bits 3 and 5, undocumented but exact. */
#define Z80_FC  0x01u
#define Z80_FN  0x02u
#define Z80_FPV 0x04u
#define Z80_FX  0x08u
#define Z80_FH  0x10u
#define Z80_FY  0x20u
#define Z80_FZ  0x40u
#define Z80_FS  0x80u

/* One page of the address space: an indexed load for each direction, or
 * NULL for the bus's slow path. Exactly two pointers (CLAUDE.md); per-page
 * flags belong in a separate array. */
typedef struct {
    const uint8_t *read;
    uint8_t       *write;
} page_t;

#define Z80_PAGE_COUNT 256u

typedef struct {
    const page_t *page;          /* Z80_PAGE_COUNT entries                */
    void         *ctx;           /* passed to every function below        */
    uint8_t (*mem_read)(void *ctx, uint16_t addr);   /* page read  NULL   */
    void    (*mem_write)(void *ctx, uint16_t addr, uint8_t v); /* write NULL */
    uint8_t (*io_read)(void *ctx, uint16_t port);    /* the full 16 bits  */
    void    (*io_write)(void *ctx, uint16_t port, uint8_t v);

    /* Optional, NULL for none: a PC whose low byte is non-zero here is
     * offered to trap() at the instruction boundary before it, unless an
     * interrupt is due there. True stalls the CPU as if WAIT were held:
     * the rest of the run passes with no instruction executed, and the
     * next run offers the PC again (design.md §10.3). */
    const uint8_t *trap_lo;
    bool    (*trap)(void *ctx);
} z80_bus_t;

/* A register pair, addressable as a word or as its two halves. */
typedef union {
    uint16_t w;
    struct { uint8_t l, h; } b;
} z80_pair_t;

typedef struct {
    z80_pair_t af, bc, de, hl;
    z80_pair_t ix, iy;
    z80_pair_t wz;               /* MEMPTR                                */
    uint16_t   sp, pc;
    uint16_t   af_, bc_, de_, hl_;   /* the alternate set                 */
    uint8_t    i;
    uint8_t    r;                /* bits 0-6 count M1 cycles; bit 7 is r7 */
    uint8_t    r7;               /* bit 7 of R, as last loaded by LD R,A  */
    uint8_t    iff1, iff2, im;
    uint8_t    q;                /* F if the last instruction wrote it, else 0 */

    bool       halted;           /* PC stays on the HALT while set        */
    bool       int_blocked;      /* after EI or a lone prefix: no INT next */
    uint8_t    prefix;           /* a DD/FD fetched after another, or 0   */
    bool       ld_a_ir;          /* the last instruction was LD A,I/LD A,R */
    bool       int_line;         /* INT, a level (§5.3)                   */
    bool       nmi_pending;      /* NMI, edge-triggered and latched       */
    uint8_t    int_data;         /* bus byte during INT acknowledge (§16) */

    uint32_t   t;                /* T-states, wrapping                    */
    uint32_t   insns;            /* instructions run, wrapping (§14)      */
    uint32_t   ed_holes;         /* ED opcodes that act as NOPs (§5.1)    */

    z80_bus_t  bus;
} z80_t;

/* Power-on state: PC, I, R and IM zero, IFF1/IFF2 clear; AF and SP $FFFF,
 * the other registers $FFFF as well. Keeps the bus. */
void     z80_reset(z80_t *c);

/* Run whole instructions until at least t_states have passed, and return
 * the T-states actually run (design.md §4.2: the caller carries the
 * overshoot). An interrupt acceptance counts as an instruction. A trap
 * that stalls ends the run at exactly t_states. */
uint32_t z80_run(z80_t *c, uint32_t t_states);

/* One instruction, or one interrupt acceptance, which also counts in
 * insns. Returns its T-states. */
uint32_t z80_step(z80_t *c);

/* The INT line is a level; NMI latches on a rising edge. */
static inline void z80_set_int(z80_t *c, bool asserted) { c->int_line = asserted; }
void     z80_nmi(z80_t *c);

/* R as LD A,R reads it. */
static inline uint8_t z80_r(const z80_t *c) {
    return (uint8_t)((c->r & 0x7Fu) | (c->r7 & 0x80u));
}

#endif /* PICO_ACE_Z80_H */
