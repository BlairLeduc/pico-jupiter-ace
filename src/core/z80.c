/* z80.c — Zilog Z80 interpreter (design.md §5). See z80.h.
 *
 * One switch per prefix page (§5.2). DD and FD share one body,
 * parametrised by the index register, that handles only the opcodes the
 * prefix changes and hands every other one to the unprefixed page.
 *
 * Sources for the undocumented behaviour: Sean Young, "The Undocumented
 * Z80 Documented" (flags, DDCB copies, block I/O flags); boo_boo and
 * Vladimir Kladov, "memptr_eng.txt" (MEMPTR); Patrik Rak's SCF/CCF
 * finding (Q). FUSE's tests are the arbiter where these disagree (§5.1).
 */
#include "z80.h"

#include <stddef.h>

#include "hot.h"

/* Registers by name. Every function below calls its CPU `c`. */
#define A  (c->af.b.h)
#define F  (c->af.b.l)
#define B  (c->bc.b.h)
#define C  (c->bc.b.l)
#define D  (c->de.b.h)
#define E  (c->de.b.l)
#define H  (c->hl.b.h)
#define L  (c->hl.b.l)
#define AF (c->af.w)
#define BC (c->bc.w)
#define DE (c->de.w)
#define HL (c->hl.w)
#define SP (c->sp)
#define PC (c->pc)
#define WZ (c->wz.w)

#define FC  Z80_FC
#define FN  Z80_FN
#define FPV Z80_FPV
#define FH  Z80_FH
#define FZ  Z80_FZ
#define FS  Z80_FS
#define FXY (Z80_FX | Z80_FY)
#define FSZPV (FS | FZ | FPV)

#define T(n) (c->t += (n))

/* Every write of F goes through SETF, so Q is F when this instruction
 * wrote the flags and 0 when it did not (SCF and CCF read it). */
#define SETF(v) (F = c->q = (uint8_t)(v))

/* PV set for even parity, by the classic doubling construction. */
#define P2(n) n, n ^ 4, n ^ 4, n
#define P4(n) P2(n), P2(n ^ 4), P2(n ^ 4), P2(n)
#define P6(n) P4(n), P4(n ^ 4), P4(n ^ 4), P4(n)
static const uint8_t parity[256] = { P6(4), P6(0), P6(0), P6(4) };
#undef P2
#undef P4
#undef P6

static inline uint8_t sz53(uint8_t v) {
    return (uint8_t)((v & (FS | FXY)) | (v == 0 ? FZ : 0));
}

static inline uint8_t sz53p(uint8_t v) { return (uint8_t)(sz53(v) | parity[v]); }

/* ---- Bus ------------------------------------------------------------- */

/* GCC keeps out-of-line copies of rd, imm16, push16 and pop16 at -O3, so
 * they are marked to follow the interpreter into SRAM; without that, a
 * tier-2 image calls them in flash through veneers (hot.h). */

static inline uint8_t ACE_HOT2(rd)(z80_t *c, uint16_t a) {
    const uint8_t *p = c->bus.page[a >> 8].read;
    if (__builtin_expect(p != NULL, 1))
        return p[a & 0xFFu];
    return c->bus.mem_read(c->bus.ctx, a);
}

static inline void wr(z80_t *c, uint16_t a, uint8_t v) {
    uint8_t *p = c->bus.page[a >> 8].write;
    if (__builtin_expect(p != NULL, 1))
        p[a & 0xFFu] = v;
    else
        c->bus.mem_write(c->bus.ctx, a, v);
}

static inline uint8_t in(z80_t *c, uint16_t port) {
    return c->bus.io_read(c->bus.ctx, port);
}

static inline void out(z80_t *c, uint16_t port, uint8_t v) {
    c->bus.io_write(c->bus.ctx, port, v);
}

/* An opcode fetch: an M1 cycle, which counts in R. */
static inline uint8_t m1(z80_t *c) {
    c->r++;
    return rd(c, PC++);
}

static inline uint8_t imm8(z80_t *c) { return rd(c, PC++); }

static inline uint16_t ACE_HOT2(imm16)(z80_t *c) {
    uint8_t lo = imm8(c);
    return (uint16_t)(lo | (imm8(c) << 8));
}

static inline uint16_t rd16(z80_t *c, uint16_t a) {
    uint8_t lo = rd(c, a);
    return (uint16_t)(lo | (rd(c, (uint16_t)(a + 1)) << 8));
}

static inline void wr16(z80_t *c, uint16_t a, uint16_t v) {
    wr(c, a, (uint8_t)v);
    wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static inline void ACE_HOT2(push16)(z80_t *c, uint16_t v) {
    wr(c, --SP, (uint8_t)(v >> 8));
    wr(c, --SP, (uint8_t)v);
}

static inline uint16_t ACE_HOT2(pop16)(z80_t *c) {
    uint8_t lo = rd(c, SP++);
    return (uint16_t)(lo | (rd(c, SP++) << 8));
}

/* ---- Register fields: r = B C D E H L (HL) A, rp = BC DE HL SP ------- */

static inline uint8_t get_r(z80_t *c, int i) {
    switch (i) {
    case 0: return B;
    case 1: return C;
    case 2: return D;
    case 3: return E;
    case 4: return H;
    case 5: return L;
    default: return A;
    }
}

static inline void set_r(z80_t *c, int i, uint8_t v) {
    switch (i) {
    case 0: B = v; break;
    case 1: C = v; break;
    case 2: D = v; break;
    case 3: E = v; break;
    case 4: H = v; break;
    case 5: L = v; break;
    default: A = v; break;
    }
}

static inline uint16_t get_rp(z80_t *c, int p) {
    switch (p) {
    case 0: return BC;
    case 1: return DE;
    case 2: return HL;
    default: return SP;
    }
}

static inline void set_rp(z80_t *c, int p, uint16_t v) {
    switch (p) {
    case 0: BC = v; break;
    case 1: DE = v; break;
    case 2: HL = v; break;
    default: SP = v; break;
    }
}

/* NZ Z NC C PO PE P M */
static inline bool cond(z80_t *c, int k) {
    switch (k) {
    case 0: return !(F & FZ);
    case 1: return (F & FZ) != 0;
    case 2: return !(F & FC);
    case 3: return (F & FC) != 0;
    case 4: return !(F & FPV);
    case 5: return (F & FPV) != 0;
    case 6: return !(F & FS);
    default: return (F & FS) != 0;
    }
}

/* ---- ALU ------------------------------------------------------------- */

static inline void add8(z80_t *c, uint8_t v, unsigned carry) {
    unsigned r = A + v + carry;
    SETF(sz53((uint8_t)r) | ((r >> 8) & FC) | ((A ^ v ^ r) & FH) |
         (((A ^ ~v) & (A ^ r) & 0x80u) >> 5));
    A = (uint8_t)r;
}

static inline uint8_t sub8(z80_t *c, uint8_t v, unsigned carry) {
    unsigned r = A - v - carry;
    SETF(sz53((uint8_t)r) | FN | ((r >> 8) & FC) | ((A ^ v ^ r) & FH) |
         (((A ^ v) & (A ^ r) & 0x80u) >> 5));
    return (uint8_t)r;
}

/* ADD ADC SUB SBC AND XOR OR CP */
static inline void alu(z80_t *c, int k, uint8_t v) {
    switch (k) {
    case 0: add8(c, v, 0); break;
    case 1: add8(c, v, F & FC); break;
    case 2: A = sub8(c, v, 0); break;
    case 3: A = sub8(c, v, F & FC); break;
    case 4: A &= v; SETF(sz53p(A) | FH); break;
    case 5: A ^= v; SETF(sz53p(A)); break;
    case 6: A |= v; SETF(sz53p(A)); break;
    default:
        sub8(c, v, 0);                      /* X and Y from the operand */
        SETF((F & ~FXY) | (v & FXY));
        break;
    }
}

static inline uint8_t inc8(z80_t *c, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    SETF((F & FC) | sz53(r) | ((r & 0x0F) == 0 ? FH : 0) | (r == 0x80 ? FPV : 0));
    return r;
}

static inline uint8_t dec8(z80_t *c, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    SETF((F & FC) | FN | sz53(r) | ((v & 0x0F) == 0 ? FH : 0) | (r == 0x7F ? FPV : 0));
    return r;
}

static inline uint16_t add16(z80_t *c, uint16_t a, uint16_t v) {
    uint32_t r = (uint32_t)a + v;
    WZ = (uint16_t)(a + 1);
    SETF((F & FSZPV) | ((r >> 16) & FC) | ((r >> 8) & FXY) | (((a ^ v ^ r) >> 8) & FH));
    return (uint16_t)r;
}

static inline void adc16(z80_t *c, uint16_t v) {
    uint16_t a = HL;
    uint32_t r = (uint32_t)a + v + (F & FC);
    WZ = (uint16_t)(a + 1);
    SETF(((r >> 8) & (FS | FXY)) | ((r & 0xFFFFu) == 0 ? FZ : 0) | ((r >> 16) & FC) |
         (((a ^ v ^ r) >> 8) & FH) | ((~(a ^ v) & (a ^ r) & 0x8000u) >> 13));
    HL = (uint16_t)r;
}

static inline void sbc16(z80_t *c, uint16_t v) {
    uint16_t a = HL;
    uint32_t r = (uint32_t)a - v - (F & FC);
    WZ = (uint16_t)(a + 1);
    SETF(((r >> 8) & (FS | FXY)) | ((r & 0xFFFFu) == 0 ? FZ : 0) | ((r >> 16) & FC) | FN |
         (((a ^ v ^ r) >> 8) & FH) | (((a ^ v) & (a ^ r) & 0x8000u) >> 13));
    HL = (uint16_t)r;
}

static inline void daa(z80_t *c) {
    uint8_t a = A, add = 0, cy = F & FC, h;
    if ((F & FH) || (a & 0x0F) > 9)
        add = 0x06;
    if (cy || a > 0x99) {
        add |= 0x60;
        cy = FC;
    }
    if (F & FN) {
        A = (uint8_t)(a - add);
        h = ((F & FH) && (a & 0x0F) < 6) ? FH : 0;
    } else {
        A = (uint8_t)(a + add);
        h = (a & 0x0F) > 9 ? FH : 0;
    }
    SETF(sz53p(A) | (F & FN) | cy | h);
}

/* RLC RRC RL RR SLA SRA SLL SRL */
static inline uint8_t rot(z80_t *c, int k, uint8_t v) {
    uint8_t r, cy;
    switch (k) {
    case 0: cy = v >> 7; r = (uint8_t)((v << 1) | cy); break;
    case 1: cy = v & 1; r = (uint8_t)((v >> 1) | (cy << 7)); break;
    case 2: cy = v >> 7; r = (uint8_t)((v << 1) | (F & FC)); break;
    case 3: cy = v & 1; r = (uint8_t)((v >> 1) | ((F & FC) << 7)); break;
    case 4: cy = v >> 7; r = (uint8_t)(v << 1); break;
    case 5: cy = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;
    case 6: cy = v >> 7; r = (uint8_t)((v << 1) | 1); break;
    default: cy = v & 1; r = v >> 1; break;
    }
    SETF(sz53p(r) | cy);
    return r;
}

/* BIT y,v. X and Y come from `xy`: the register itself, or MEMPTR's high
 * byte for the memory forms. */
static inline void bit(z80_t *c, int y, uint8_t v, uint8_t xy) {
    uint8_t m = (uint8_t)(v & (1u << y));
    SETF((F & FC) | FH | (xy & FXY) | (m ? (m & FS) : (FZ | FPV)));
}

/* rot, BIT (k = 1, not handled here), RES, SET by the CB page's top bits. */
static inline uint8_t cb_op(z80_t *c, int k, int y, uint8_t v) {
    if (k == 0)
        return rot(c, y, v);
    if (k == 2)
        return (uint8_t)(v & ~(1u << y));
    return (uint8_t)(v | (1u << y));
}

/* ---- Pages ----------------------------------------------------------- */

static void exec_main(z80_t *c, uint8_t op, uint8_t q);

static void ACE_HOT2(exec_cb)(z80_t *c) {
    uint8_t op = m1(c);
    int k = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (z == 6) {
        uint8_t v = rd(c, HL);
        if (k == 1) {
            bit(c, y, v, c->wz.b.h);
            T(12);
        } else {
            wr(c, HL, cb_op(c, k, y, v));
            T(15);
        }
        return;
    }
    uint8_t v = get_r(c, z);
    if (k == 1)
        bit(c, y, v, v);
    else
        set_r(c, z, cb_op(c, k, y, v));
    T(8);
}

/* DDCB/FDCB: d comes before the opcode, and neither is an M1. Every form
 * but BIT also copies its result into register z (§5.1). */
static void ACE_HOT2(exec_xycb)(z80_t *c, z80_pair_t *xy) {
    uint16_t a = (uint16_t)(xy->w + (int8_t)imm8(c));
    uint8_t op = imm8(c);
    int k = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = rd(c, a);
    WZ = a;
    if (k == 1) {
        bit(c, y, v, (uint8_t)(a >> 8));
        T(16);
        return;
    }
    v = cb_op(c, k, y, v);
    wr(c, a, v);
    if (z != 6)
        set_r(c, z, v);
    T(19);
}

/* Block instructions: dir is +1 for the I forms, -1 for the D forms. */
static inline void ldx(z80_t *c, int dir) {
    uint8_t v = rd(c, HL);
    wr(c, DE, v);
    HL = (uint16_t)(HL + dir);
    DE = (uint16_t)(DE + dir);
    BC--;
    uint8_t n = (uint8_t)(v + A);
    SETF((F & (FS | FZ | FC)) | (BC ? FPV : 0) | (n & Z80_FX) | ((n << 4) & Z80_FY));
}

static inline void cpx(z80_t *c, int dir) {
    uint8_t v = rd(c, HL);
    uint8_t r = (uint8_t)(A - v);
    uint8_t h = (A ^ v ^ r) & FH;
    uint8_t n = (uint8_t)(r - (h >> 4));
    HL = (uint16_t)(HL + dir);
    BC--;
    WZ = (uint16_t)(WZ + dir);
    SETF((F & FC) | FN | h | (r & FS) | (r == 0 ? FZ : 0) | (BC ? FPV : 0) |
         (n & Z80_FX) | ((n << 4) & Z80_FY));
}

/* The flags INI, IND, OUTI and OUTD share; k is the byte moved plus C+1,
 * C-1 or L (Young §4.3). */
static inline void iox_flags(z80_t *c, uint8_t v, unsigned k) {
    SETF(sz53(B) | ((v & 0x80) ? FN : 0) | (k > 0xFF ? (FH | FC) : 0) |
         parity[(uint8_t)((k & 7) ^ B)]);
}

static inline void inx(z80_t *c, int dir) {
    uint8_t v = in(c, BC);
    WZ = (uint16_t)(BC + dir);
    B--;
    wr(c, HL, v);
    HL = (uint16_t)(HL + dir);
    iox_flags(c, v, v + (uint8_t)(C + dir));
}

static inline void outx(z80_t *c, int dir) {
    uint8_t v = rd(c, HL);
    B--;
    out(c, BC, v);
    HL = (uint16_t)(HL + dir);
    WZ = (uint16_t)(BC + dir);
    iox_flags(c, v, v + L);
}

/* A repeating block instruction runs again from its own address. FUSE
 * keeps the single instruction's flags while repeating, and so does this;
 * the later finding that repeats change H, PV, X and Y is not modelled
 * (design.md §5.1). */
static inline void repeat(z80_t *c, bool again, bool memptr) {
    if (again) {
        PC -= 2;
        if (memptr)
            WZ = (uint16_t)(PC + 1);
        T(21);
    } else {
        T(16);
    }
}

static void ACE_HOT2(exec_ed)(z80_t *c) {
    uint8_t op = m1(c);
    int y = (op >> 3) & 7, p = y >> 1;

    if (op >= 0x40 && op < 0x80) {
        switch (op & 7) {
        case 0: {                                   /* IN r,(C) */
            uint8_t v = in(c, BC);
            WZ = (uint16_t)(BC + 1);
            if (y != 6)
                set_r(c, y, v);                     /* IN (C) sets flags only */
            SETF((F & FC) | sz53p(v));
            T(12);
            return;
        }
        case 1:                                     /* OUT (C),r */
            out(c, BC, y == 6 ? 0 : get_r(c, y));
            WZ = (uint16_t)(BC + 1);
            T(12);
            return;
        case 2:
            if (y & 1)
                adc16(c, get_rp(c, p));
            else
                sbc16(c, get_rp(c, p));
            T(15);
            return;
        case 3: {
            uint16_t nn = imm16(c);
            if (y & 1)
                set_rp(c, p, rd16(c, nn));
            else
                wr16(c, nn, get_rp(c, p));
            WZ = (uint16_t)(nn + 1);
            T(20);
            return;
        }
        case 4: {                                   /* NEG, and its mirrors */
            uint8_t v = A;
            A = 0;
            A = sub8(c, v, 0);
            T(8);
            return;
        }
        case 5:                                     /* RETN; RETI is the same here */
            c->iff1 = c->iff2;
            PC = pop16(c);
            WZ = PC;
            T(14);
            return;
        case 6: {                                   /* IM 0, 0/1 (as 0), 1, 2 */
            static const uint8_t mode[4] = { 0, 0, 1, 2 };
            c->im = mode[y & 3];
            T(8);
            return;
        }
        default:
            switch (y) {
            case 0: c->i = A; T(9); return;         /* LD I,A */
            case 1: c->r = A; c->r7 = A; T(9); return;  /* LD R,A */
            case 2:                                 /* LD A,I */
                A = c->i;
                SETF((F & FC) | sz53(A) | (c->iff2 ? FPV : 0));
                c->ld_a_ir = true;
                T(9);
                return;
            case 3:                                 /* LD A,R */
                A = z80_r(c);
                SETF((F & FC) | sz53(A) | (c->iff2 ? FPV : 0));
                c->ld_a_ir = true;
                T(9);
                return;
            case 4: {                               /* RRD */
                uint8_t v = rd(c, HL);
                wr(c, HL, (uint8_t)((A << 4) | (v >> 4)));
                A = (uint8_t)((A & 0xF0) | (v & 0x0F));
                WZ = (uint16_t)(HL + 1);
                SETF((F & FC) | sz53p(A));
                T(18);
                return;
            }
            case 5: {                               /* RLD */
                uint8_t v = rd(c, HL);
                wr(c, HL, (uint8_t)((v << 4) | (A & 0x0F)));
                A = (uint8_t)((A & 0xF0) | (v >> 4));
                WZ = (uint16_t)(HL + 1);
                SETF((F & FC) | sz53p(A));
                T(18);
                return;
            }
            default:                                /* ED 77, ED 7F */
                break;
            }
            break;
        }
    } else {
        switch (op) {
        case 0xA0: ldx(c, 1); T(16); return;
        case 0xA8: ldx(c, -1); T(16); return;
        case 0xB0: ldx(c, 1); repeat(c, BC != 0, true); return;
        case 0xB8: ldx(c, -1); repeat(c, BC != 0, true); return;
        case 0xA1: cpx(c, 1); T(16); return;
        case 0xA9: cpx(c, -1); T(16); return;
        case 0xB1: cpx(c, 1); repeat(c, BC != 0 && !(F & FZ), true); return;
        case 0xB9: cpx(c, -1); repeat(c, BC != 0 && !(F & FZ), true); return;
        case 0xA2: inx(c, 1); T(16); return;
        case 0xAA: inx(c, -1); T(16); return;
        case 0xB2: inx(c, 1); repeat(c, B != 0, false); return;
        case 0xBA: inx(c, -1); repeat(c, B != 0, false); return;
        case 0xA3: outx(c, 1); T(16); return;
        case 0xAB: outx(c, -1); T(16); return;
        case 0xB3: outx(c, 1); repeat(c, B != 0, false); return;
        case 0xBB: outx(c, -1); repeat(c, B != 0, false); return;
        default: break;
        }
    }

    /* A hole: two instructions' worth of NOP (§5.1). */
    c->ed_holes++;
    T(8);
}

/* The DD and FD pages. xy is IX or IY; the prefix's own M1 is already
 * counted in R, and its 4 T are added here. */
static void ACE_HOT2(exec_xy)(z80_t *c, z80_pair_t *xy, uint8_t q) {
#define XY  (xy->w)
#define XH  (xy->b.h)
#define XL  (xy->b.l)
/* (IX+d): reads d and sets MEMPTR. */
#define EA() (WZ = (uint16_t)(XY + (int8_t)imm8(c)))
/* Register i, with H and L replaced by the index register's halves. */
#define GET_X(i) ((i) == 4 ? XH : (i) == 5 ? XL : get_r(c, i))
#define SET_X(i, v) do { if ((i) == 4) XH = (v); else if ((i) == 5) XL = (v); \
                         else set_r(c, i, v); } while (0)

    T(4);
    uint8_t op = m1(c);
    switch (op) {
    case 0x09: XY = add16(c, XY, BC); T(11); break;
    case 0x19: XY = add16(c, XY, DE); T(11); break;
    case 0x29: XY = add16(c, XY, XY); T(11); break;
    case 0x39: XY = add16(c, XY, SP); T(11); break;
    case 0x21: XY = imm16(c); T(10); break;
    case 0x22: { uint16_t nn = imm16(c); wr16(c, nn, XY); WZ = (uint16_t)(nn + 1); T(16); break; }
    case 0x2A: { uint16_t nn = imm16(c); XY = rd16(c, nn); WZ = (uint16_t)(nn + 1); T(16); break; }
    case 0x23: XY++; T(6); break;
    case 0x2B: XY--; T(6); break;
    case 0x24: XH = inc8(c, XH); T(4); break;
    case 0x25: XH = dec8(c, XH); T(4); break;
    case 0x26: XH = imm8(c); T(7); break;
    case 0x2C: XL = inc8(c, XL); T(4); break;
    case 0x2D: XL = dec8(c, XL); T(4); break;
    case 0x2E: XL = imm8(c); T(7); break;
    case 0x34: { uint16_t a = EA(); wr(c, a, inc8(c, rd(c, a))); T(19); break; }
    case 0x35: { uint16_t a = EA(); wr(c, a, dec8(c, rd(c, a))); T(19); break; }
    case 0x36: { uint16_t a = EA(); wr(c, a, imm8(c)); T(15); break; }

/* LD r,r' with IXH/IXL; the (IX+d) forms use the real H and L. */
#define XLD(d, s) case 0x40 + 8 * (d) + (s): SET_X(d, GET_X(s)); T(4); break;
#define XLD_ROW(d) XLD(d, 0) XLD(d, 1) XLD(d, 2) XLD(d, 3) XLD(d, 4) XLD(d, 5) XLD(d, 7) \
    case 0x46 + 8 * (d): { uint16_t a = EA(); set_r(c, d, rd(c, a)); T(15); break; } \
    case 0x70 + (d): { uint16_t a = EA(); wr(c, a, get_r(c, d)); T(15); break; }
    XLD_ROW(0) XLD_ROW(1) XLD_ROW(2) XLD_ROW(3) XLD_ROW(4) XLD_ROW(5) XLD_ROW(7)
#undef XLD_ROW
#undef XLD

#define XALU_ROW(k) \
    case 0x84 + 8 * (k): alu(c, k, XH); T(4); break; \
    case 0x85 + 8 * (k): alu(c, k, XL); T(4); break; \
    case 0x86 + 8 * (k): { uint16_t a = EA(); alu(c, k, rd(c, a)); T(15); break; }
    XALU_ROW(0) XALU_ROW(1) XALU_ROW(2) XALU_ROW(3)
    XALU_ROW(4) XALU_ROW(5) XALU_ROW(6) XALU_ROW(7)
#undef XALU_ROW

    case 0xCB: exec_xycb(c, xy); break;
    case 0xE1: XY = pop16(c); T(10); break;
    case 0xE5: push16(c, XY); T(11); break;
    case 0xE9: PC = XY; T(4); break;
    case 0xF9: SP = XY; T(6); break;
    case 0xE3: {
        uint16_t v = rd16(c, SP);
        wr(c, (uint16_t)(SP + 1), XH);
        wr(c, SP, XL);
        XY = WZ = v;
        T(19);
        break;
    }
    case 0xDD:
    case 0xFD:
        /* This prefix was a 4 T NOP. The next one is fetched, and runs as
         * the next step, which no interrupt may come before. */
        c->prefix = op;
        c->int_blocked = true;
        break;
    default:
        exec_main(c, op, q);                    /* unaffected: a 4 T prefix */
        break;
    }
#undef SET_X
#undef GET_X
#undef EA
#undef XL
#undef XH
#undef XY
}

/* The unprefixed page. q is Q as the previous instruction left it. */
static void ACE_HOT2(exec_main)(z80_t *c, uint8_t op, uint8_t q) {
    switch (op) {
    case 0x00: T(4); break;                                   /* NOP */

/* LD rp,nn; INC rp; DEC rp; ADD HL,rp */
#define RP_ROW(p) \
    case 0x01 + 16 * (p): set_rp(c, p, imm16(c)); T(10); break; \
    case 0x03 + 16 * (p): set_rp(c, p, (uint16_t)(get_rp(c, p) + 1)); T(6); break; \
    case 0x0B + 16 * (p): set_rp(c, p, (uint16_t)(get_rp(c, p) - 1)); T(6); break; \
    case 0x09 + 16 * (p): HL = add16(c, HL, get_rp(c, p)); T(11); break;
    RP_ROW(0) RP_ROW(1) RP_ROW(2) RP_ROW(3)
#undef RP_ROW

/* INC r; DEC r; LD r,n */
#define R8_ROW(i) \
    case 0x04 + 8 * (i): set_r(c, i, inc8(c, get_r(c, i))); T(4); break; \
    case 0x05 + 8 * (i): set_r(c, i, dec8(c, get_r(c, i))); T(4); break; \
    case 0x06 + 8 * (i): set_r(c, i, imm8(c)); T(7); break;
    R8_ROW(0) R8_ROW(1) R8_ROW(2) R8_ROW(3) R8_ROW(4) R8_ROW(5) R8_ROW(7)
#undef R8_ROW
    case 0x34: { uint8_t v = inc8(c, rd(c, HL)); wr(c, HL, v); T(11); break; }
    case 0x35: { uint8_t v = dec8(c, rd(c, HL)); wr(c, HL, v); T(11); break; }
    case 0x36: wr(c, HL, imm8(c)); T(10); break;

    case 0x02: wr(c, BC, A); WZ = (uint16_t)(((BC + 1) & 0xFF) | (A << 8)); T(7); break;
    case 0x12: wr(c, DE, A); WZ = (uint16_t)(((DE + 1) & 0xFF) | (A << 8)); T(7); break;
    case 0x0A: A = rd(c, BC); WZ = (uint16_t)(BC + 1); T(7); break;
    case 0x1A: A = rd(c, DE); WZ = (uint16_t)(DE + 1); T(7); break;
    case 0x22: { uint16_t nn = imm16(c); wr16(c, nn, HL); WZ = (uint16_t)(nn + 1); T(16); break; }
    case 0x2A: { uint16_t nn = imm16(c); HL = rd16(c, nn); WZ = (uint16_t)(nn + 1); T(16); break; }
    case 0x32: {
        uint16_t nn = imm16(c);
        wr(c, nn, A);
        WZ = (uint16_t)(((nn + 1) & 0xFF) | (A << 8));
        T(13);
        break;
    }
    case 0x3A: { uint16_t nn = imm16(c); A = rd(c, nn); WZ = (uint16_t)(nn + 1); T(13); break; }

    case 0x07:                                                /* RLCA */
        A = (uint8_t)((A << 1) | (A >> 7));
        SETF((F & FSZPV) | (A & (FXY | FC)));
        T(4);
        break;
    case 0x0F: {                                              /* RRCA */
        uint8_t cy = A & 1;
        A = (uint8_t)((A >> 1) | (cy << 7));
        SETF((F & FSZPV) | (A & FXY) | cy);
        T(4);
        break;
    }
    case 0x17: {                                              /* RLA */
        uint8_t cy = A >> 7;
        A = (uint8_t)((A << 1) | (F & FC));
        SETF((F & FSZPV) | (A & FXY) | cy);
        T(4);
        break;
    }
    case 0x1F: {                                              /* RRA */
        uint8_t cy = A & 1;
        A = (uint8_t)((A >> 1) | ((F & FC) << 7));
        SETF((F & FSZPV) | (A & FXY) | cy);
        T(4);
        break;
    }
    case 0x27: daa(c); T(4); break;
    case 0x2F:                                                /* CPL */
        A = (uint8_t)~A;
        SETF((F & (FSZPV | FC)) | FH | FN | (A & FXY));
        T(4);
        break;
    case 0x37:                                                /* SCF */
        SETF((F & FSZPV) | (((q ^ F) | A) & FXY) | FC);
        T(4);
        break;
    case 0x3F:                                                /* CCF */
        SETF((F & FSZPV) | ((F & FC) ? FH : FC) | (((q ^ F) | A) & FXY));
        T(4);
        break;

    case 0x08: { uint16_t v = AF; AF = c->af_; c->af_ = v; T(4); break; }
    case 0xD9: {                                              /* EXX */
        uint16_t v;
        v = BC; BC = c->bc_; c->bc_ = v;
        v = DE; DE = c->de_; c->de_ = v;
        v = HL; HL = c->hl_; c->hl_ = v;
        T(4);
        break;
    }
    case 0xEB: { uint16_t v = DE; DE = HL; HL = v; T(4); break; }
    case 0xE3: {                                              /* EX (SP),HL */
        uint16_t v = rd16(c, SP);
        wr(c, (uint16_t)(SP + 1), H);
        wr(c, SP, L);
        HL = WZ = v;
        T(19);
        break;
    }

/* DJNZ and JR. A branch not taken skips its displacement without reading
 * it, as FUSE does. A real Z80 does read it, but a read with no side
 * effects is invisible, and FUSE's access order is the one checked. */
    case 0x10:                                                /* DJNZ */
        if (--B) {
            PC = (uint16_t)(PC + 1 + (int8_t)rd(c, PC));
            WZ = PC;
            T(13);
        } else {
            PC++;
            T(8);
        }
        break;
#define JR(op_, taken) case op_: \
        if (taken) { PC = (uint16_t)(PC + 1 + (int8_t)rd(c, PC)); WZ = PC; T(12); } \
        else { PC++; T(7); } \
        break;
    JR(0x18, true)
    JR(0x20, cond(c, 0))
    JR(0x28, cond(c, 1))
    JR(0x30, cond(c, 2))
    JR(0x38, cond(c, 3))
#undef JR

    case 0x76:                                                /* HALT */
        /* PC stays on the HALT, which runs again as a NOP until an
         * interrupt steps past it (FUSE's model; the visible state is the
         * same as a CPU that fetches from PC+1 and ignores it). */
        c->halted = true;
        PC--;
        T(4);
        break;

/* LD r,r'; LD r,(HL); LD (HL),r */
#define LD_RR(d, s) case 0x40 + 8 * (d) + (s): set_r(c, d, get_r(c, s)); T(4); break;
#define LD_ROW(d) LD_RR(d, 0) LD_RR(d, 1) LD_RR(d, 2) LD_RR(d, 3) LD_RR(d, 4) LD_RR(d, 5) LD_RR(d, 7) \
    case 0x46 + 8 * (d): set_r(c, d, rd(c, HL)); T(7); break; \
    case 0x70 + (d): wr(c, HL, get_r(c, d)); T(7); break;
    LD_ROW(0) LD_ROW(1) LD_ROW(2) LD_ROW(3) LD_ROW(4) LD_ROW(5) LD_ROW(7)
#undef LD_ROW
#undef LD_RR

/* ALU A,r; ALU A,(HL); ALU A,n */
#define ALU_R(k, s) case 0x80 + 8 * (k) + (s): alu(c, k, get_r(c, s)); T(4); break;
#define ALU_ROW(k) ALU_R(k, 0) ALU_R(k, 1) ALU_R(k, 2) ALU_R(k, 3) ALU_R(k, 4) ALU_R(k, 5) ALU_R(k, 7) \
    case 0x86 + 8 * (k): alu(c, k, rd(c, HL)); T(7); break; \
    case 0xC6 + 8 * (k): alu(c, k, imm8(c)); T(7); break;
    ALU_ROW(0) ALU_ROW(1) ALU_ROW(2) ALU_ROW(3) ALU_ROW(4) ALU_ROW(5) ALU_ROW(6) ALU_ROW(7)
#undef ALU_ROW
#undef ALU_R

/* RET cc; JP cc,nn; CALL cc,nn; RST */
#define CC_ROW(k) \
    case 0xC0 + 8 * (k): \
        if (cond(c, k)) { PC = pop16(c); WZ = PC; T(11); } else { T(5); } \
        break; \
    case 0xC2 + 8 * (k): { \
        uint16_t nn = imm16(c); WZ = nn; \
        if (cond(c, k)) PC = nn; \
        T(10); break; } \
    case 0xC4 + 8 * (k): { \
        uint16_t nn = imm16(c); WZ = nn; \
        if (cond(c, k)) { push16(c, PC); PC = nn; T(17); } else { T(10); } \
        break; } \
    case 0xC7 + 8 * (k): push16(c, PC); PC = 8 * (k); WZ = PC; T(11); break;
    CC_ROW(0) CC_ROW(1) CC_ROW(2) CC_ROW(3) CC_ROW(4) CC_ROW(5) CC_ROW(6) CC_ROW(7)
#undef CC_ROW

    case 0xC1: BC = pop16(c); T(10); break;
    case 0xD1: DE = pop16(c); T(10); break;
    case 0xE1: HL = pop16(c); T(10); break;
    case 0xF1: AF = pop16(c); T(10); break;
    case 0xC5: push16(c, BC); T(11); break;
    case 0xD5: push16(c, DE); T(11); break;
    case 0xE5: push16(c, HL); T(11); break;
    case 0xF5: push16(c, AF); T(11); break;

    case 0xC3: PC = WZ = imm16(c); T(10); break;
    case 0xC9: PC = WZ = pop16(c); T(10); break;
    case 0xCD: { uint16_t nn = imm16(c); push16(c, PC); PC = WZ = nn; T(17); break; }
    case 0xE9: PC = HL; T(4); break;
    case 0xF9: SP = HL; T(6); break;

    case 0xD3: {                                              /* OUT (n),A */
        uint8_t n = imm8(c);
        out(c, (uint16_t)(n | (A << 8)), A);
        WZ = (uint16_t)(((n + 1) & 0xFF) | (A << 8));
        T(11);
        break;
    }
    case 0xDB: {                                              /* IN A,(n) */
        uint16_t port = (uint16_t)(imm8(c) | (A << 8));
        A = in(c, port);
        WZ = (uint16_t)(port + 1);
        T(11);
        break;
    }

    case 0xF3: c->iff1 = c->iff2 = 0; T(4); break;           /* DI */
    case 0xFB:                                                /* EI */
        c->iff1 = c->iff2 = 1;
        c->int_blocked = true;                                /* §5.1 */
        T(4);
        break;

    case 0xCB: exec_cb(c); break;
    case 0xED: exec_ed(c); break;
    case 0xDD: exec_xy(c, &c->ix, q); break;
    case 0xFD: exec_xy(c, &c->iy, q); break;
    }
}

/* ---- Interrupts and the loop ------------------------------------------ */

/* Leaving HALT steps past it: the address pushed is the next one. */
static inline void wake(z80_t *c) {
    if (c->halted) {
        c->halted = false;
        PC++;
    }
}

static void accept_nmi(z80_t *c) {
    wake(c);
    c->nmi_pending = false;
    c->iff1 = 0;
    c->r++;
    c->q = 0;
    push16(c, PC);
    PC = WZ = 0x0066;
    T(11);
}

static void accept_int(z80_t *c) {
    wake(c);
    /* NMOS: an interrupt accepted straight after LD A,I or LD A,R leaves
     * P/V reading 0, not IFF2 (§5.1). */
    if (c->ld_a_ir)
        F &= (uint8_t)~FPV;
    c->iff1 = c->iff2 = 0;
    c->r++;
    c->q = 0;
    push16(c, PC);
    switch (c->im) {
    case 2:
        PC = WZ = rd16(c, (uint16_t)((c->i << 8) | c->int_data));
        T(19);
        break;
    case 1:
        PC = WZ = 0x0038;
        T(13);
        break;
    default:
        /* IM 0 executes the byte on the bus. Only an RST is modelled; on
         * the Ace that byte is $FF, RST $38, as in IM 1 (§16). */
        PC = WZ = c->int_data & 0x38u;
        T(13);
        break;
    }
}

void z80_reset(z80_t *c) {
    c->af.w = c->bc.w = c->de.w = c->hl.w = 0xFFFF;
    c->ix.w = c->iy.w = 0xFFFF;
    c->af_ = c->bc_ = c->de_ = c->hl_ = 0xFFFF;
    c->sp = 0xFFFF;
    c->pc = 0;
    c->wz.w = 0;
    c->i = c->r = c->r7 = 0;
    c->iff1 = c->iff2 = c->im = 0;
    c->q = 0;
    c->halted = false;
    c->int_blocked = false;
    c->prefix = 0;
    c->ld_a_ir = false;
    c->nmi_pending = false;
    c->int_data = 0xFF;
}

void z80_nmi(z80_t *c) { c->nmi_pending = true; }

uint32_t ACE_HOT2(z80_step)(z80_t *c) {
    uint32_t t0 = c->t;
    c->insns++;

    if (__builtin_expect(c->nmi_pending, 0) && !c->prefix) {
        accept_nmi(c);
        return c->t - t0;
    }
    if (c->int_line && c->iff1 && !c->int_blocked) {
        accept_int(c);
        return c->t - t0;
    }

    uint8_t q = c->q;
    c->q = 0;
    c->int_blocked = false;
    c->ld_a_ir = false;

    if (__builtin_expect(c->prefix != 0, 0)) {
        uint8_t p = c->prefix;
        c->prefix = 0;
        exec_xy(c, p == 0xDD ? &c->ix : &c->iy, q);
    } else {
        exec_main(c, m1(c), q);
    }
    return c->t - t0;
}

/* Would z80_step accept an interrupt rather than run an instruction? */
static inline bool int_due(const z80_t *c) {
    return c->nmi_pending || (c->int_line && c->iff1 && !c->int_blocked);
}

uint32_t ACE_HOT2(z80_run)(z80_t *c, uint32_t t_states) {
    uint32_t t0 = c->t;
    const uint8_t *lo = c->bus.trap_lo;
    if (!lo) {
        while ((uint32_t)(c->t - t0) < t_states)
            z80_step(c);
        return c->t - t0;
    }
    /* One load per instruction while traps are set (EL §8.2). An
     * interrupt due at the boundary is taken first, as the real CPU
     * would before fetching the trapped instruction. */
    while ((uint32_t)(c->t - t0) < t_states) {
        if (__builtin_expect(lo[PC & 0xFFu], 0) && !c->prefix && !c->halted &&
            !int_due(c) && c->bus.trap(c->bus.ctx)) {
            c->t = t0 + t_states;
            break;
        }
        z80_step(c);
    }
    return c->t - t0;
}
