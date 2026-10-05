/* snap_ace.c — .ace snapshots imported (snap_ace.h, design.md §10.5). */

#include "snap_ace.h"

#include <string.h>

#define DUMP_BASE  0x2000u
#define RAMTOP_AT  0x2080u    /* ACE32's RAMTOP word                     */
#define REGS_AT    0x2100u    /* AF, then a 32-bit word a register        */
#define HDR_END    0x2144u    /* one past R's word, the last one read     */
#define RAM_FIRST  0x2400u    /* the first address that is machine state  */
#define DUMP_MIN   0x4000u    /* a file holds at least the 3K machine     */

/* The end of the address space and one run more: a run may cross the
 * end of RAM (snap_ace.h). */
#define DUMP_MAX   (0x10000u + 0xFFu)

/* The registers' words from REGS_AT, in ACE32's order. */
enum { R_AF, R_BC, R_DE, R_HL, R_IX, R_IY, R_SP, R_PC, R_AF_, R_BC_, R_DE_, R_HL_,
       R_IM, R_IFF1, R_IFF2, R_I, R_R };

/* ---- the input, a buffer at a time --------------------------------------- */

typedef struct {
    snap_ace_read_fn read;
    void   *ctx;
    uint8_t buf[128];
    int     n, i;
} rd_t;

/* A byte, -1 at the end, -2 on an error. */
static int next(rd_t *r) {
    if (r->i == r->n) {
        r->n = r->read(r->ctx, r->buf, sizeof r->buf);
        r->i = 0;
        if (r->n < 0) { r->n = 0; return -2; }
        if (r->n == 0) return -1;
    }
    return r->buf[r->i++];
}

/* ---- decoding ------------------------------------------------------------ */

typedef struct {
    uint8_t  hdr[HDR_END - DUMP_BASE];
    uint32_t pos;              /* the next address                         */
    ace_t   *m;                /* written once started; NULL in the check  */
    bool     started;
    snap_ace_status_t st;
    snap_ace_info_t *info;
} dec_t;

static uint16_t word(const dec_t *d, unsigned reg) {
    const uint8_t *p = d->hdr + (REGS_AT - DUMP_BASE) + 4u * reg;
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* IM, IFF1, IFF2, I and R: the low byte only (snap_ace.h). */
static uint8_t byte(const dec_t *d, unsigned reg) {
    return d->hdr[(REGS_AT - DUMP_BASE) + 4u * reg];
}

static uint32_t ram_end(ace_ram_t ram) {
    return ACE_XRAM_BASE + ace_ram_bytes(ram) - ACE_BLOCK_BYTES;
}

/* What the header says, once it has all arrived: the machine, and the
 * fields no .ace has. Fills info. */
static snap_ace_status_t parse(dec_t *d, ace_ram_t running) {
    snap_ace_info_t *in = d->info;
    const uint8_t *t = d->hdr + (RAMTOP_AT - DUMP_BASE);
    uint16_t top = (uint16_t)(t[0] | (t[1] << 8));
    switch (top) {
    case 0x4000u: in->needs = ACE_RAM_3K;  break;
    case 0x8000u: in->needs = ACE_RAM_19K; break;
    case 0xC000u:                            /* 35K: no such machine here */
    case 0x0000u: in->needs = ACE_RAM_51K; break;
    default:      return SNAP_ACE_NOT_ACE;
    }
    in->ramtop = top ? top : 0x10000u;
    in->pc = word(d, R_PC);
    in->sp = word(d, R_SP);
    if (byte(d, R_IM) > 2u) return SNAP_ACE_NOT_ACE;
    return in->needs == running ? SNAP_ACE_OK : SNAP_ACE_OTHER_RAM;
}

/* A byte at addr into its home, if it has one in this machine. */
static void put(ace_t *m, uint32_t addr, uint8_t v) {
    if (addr >= 0x2400u && addr < 0x2800u)      m->vram[addr - 0x2400u] = v;
    else if (addr >= 0x2C00u && addr < 0x3000u) m->cram[addr - 0x2C00u] = v;
    else if (addr >= 0x3C00u && addr < 0x4000u) m->uram[addr - 0x3C00u] = v;
    else if (addr >= ACE_XRAM_BASE && addr < ram_end(m->cfg.ram))
        m->xram[addr - ACE_XRAM_BASE] = v;
}

/* The first byte of machine state is about to be written: the header is
 * whole, so the machine is checked against it, and cleared. */
static bool start(dec_t *d) {
    d->started = true;
    d->st = parse(d, d->m->cfg.ram);
    if (d->st != SNAP_ACE_OK) return false;
    ace_t *m = d->m;
    memset(m->vram, 0, sizeof m->vram);
    memset(m->cram, 0, sizeof m->cram);
    memset(m->uram, 0, sizeof m->uram);
    memset(m->xram, 0, sizeof m->xram);
    return true;
}

static bool emit(dec_t *d, uint8_t v, unsigned n) {
    for (; n; n--) {
        uint32_t a = d->pos++;
        if (a >= DUMP_MAX) {
            d->st = SNAP_ACE_NOT_ACE;
            return false;
        }
        if (a < HDR_END) d->hdr[a - DUMP_BASE] = v;
        if (d->m && a >= RAM_FIRST) {
            if (!d->started && !start(d)) return false;
            put(d->m, a, v);
        }
    }
    return true;
}

/* The whole stream, to its end mark. */
static snap_ace_status_t decode(dec_t *d, snap_ace_read_fn read, void *ctx) {
    rd_t r = { .read = read, .ctx = ctx };
    d->pos = DUMP_BASE;
    d->st = SNAP_ACE_OK;
    for (;;) {
        int b = next(&r);
        if (b < 0) return b == -2 ? SNAP_ACE_IO : SNAP_ACE_NOT_ACE;
        if (b != 0xED) {
            if (!emit(d, (uint8_t)b, 1)) return d->st;
            continue;
        }
        int n = next(&r);
        if (n < 0) return n == -2 ? SNAP_ACE_IO : SNAP_ACE_NOT_ACE;
        if (n == 0) return SNAP_ACE_OK;
        int v = next(&r);
        if (v < 0) return v == -2 ? SNAP_ACE_IO : SNAP_ACE_NOT_ACE;
        if (!emit(d, (uint8_t)v, (unsigned)n)) return d->st;
    }
}

/* With the whole file read: is its stack top in it, or is it the key
 * wait's, which the import writes back? */
static snap_ace_status_t finish(dec_t *d, ace_ram_t running) {
    snap_ace_info_t *in = d->info;
    in->end = d->pos;
    if (d->pos < DUMP_MIN) return SNAP_ACE_NOT_ACE;
    snap_ace_status_t st = parse(d, running);
    if (st == SNAP_ACE_NOT_ACE) return st;

    /* No machine loads a file without its stack, so that is said before
     * which machine it needs. */
    uint32_t held = d->pos < ram_end(in->needs) ? d->pos : ram_end(in->needs);
    in->repaired = false;
    if (in->sp >= 0x3C00u && (uint32_t)in->sp + 2u <= held) return st;
    if ((in->pc == SNAP_ACE_WAIT_PC1 || in->pc == SNAP_ACE_WAIT_PC2) &&
        word(d, R_HL) == SNAP_ACE_WAIT_HL && (uint32_t)in->sp + 2u == in->ramtop) {
        in->repaired = true;
        return st;
    }
    return SNAP_ACE_NO_STACK;
}

/* ---- the calls ------------------------------------------------------------- */

static dec_t s_dec;   /* 324 bytes of header: not on the caller's stack */

snap_ace_status_t snap_ace_check(const ace_t *m, snap_ace_read_fn read, void *ctx,
                                 snap_ace_info_t *info) {
    memset(info, 0, sizeof *info);
    memset(&s_dec, 0, sizeof s_dec);
    s_dec.info = info;
    if (m->tape.op != TAPE_NONE) return SNAP_ACE_BUSY;
    snap_ace_status_t st = decode(&s_dec, read, ctx);
    if (st != SNAP_ACE_OK) {
        info->end = s_dec.pos;
        return st;
    }
    return finish(&s_dec, m->cfg.ram);
}

snap_ace_status_t snap_ace_load(ace_t *m, snap_ace_read_fn read, void *ctx,
                                snap_ace_info_t *info) {
    memset(info, 0, sizeof *info);
    memset(&s_dec, 0, sizeof s_dec);
    s_dec.info = info;
    s_dec.m = m;
    if (m->tape.op != TAPE_NONE) return SNAP_ACE_BUSY;
    snap_ace_status_t st = decode(&s_dec, read, ctx);
    if (st == SNAP_ACE_OK && !s_dec.started) st = SNAP_ACE_NOT_ACE;
    if (st == SNAP_ACE_OK) st = finish(&s_dec, m->cfg.ram);
    if (st != SNAP_ACE_OK) return st;

    const dec_t *d = &s_dec;
    if (info->repaired) {
        put(m, info->sp, (uint8_t)SNAP_ACE_WAIT_RET);
        put(m, (uint32_t)info->sp + 1u, (uint8_t)(SNAP_ACE_WAIT_RET >> 8));
    }

    /* The CPU as ACE32 keeps it. MEMPTR and Q are not in the file; zero is
     * what a reset leaves. */
    z80_t *c = &m->cpu;
    c->af.w = word(d, R_AF);
    c->bc.w = word(d, R_BC);
    c->de.w = word(d, R_DE);
    c->hl.w = word(d, R_HL);
    c->ix.w = word(d, R_IX);
    c->iy.w = word(d, R_IY);
    c->sp = info->sp;
    c->pc = info->pc;
    c->af_ = word(d, R_AF_);
    c->bc_ = word(d, R_BC_);
    c->de_ = word(d, R_DE_);
    c->hl_ = word(d, R_HL_);
    c->im = byte(d, R_IM);
    c->iff1 = byte(d, R_IFF1) ? 1u : 0u;
    c->iff2 = byte(d, R_IFF2) ? 1u : 0u;
    c->i = byte(d, R_I);
    c->r = byte(d, R_R);
    c->r7 = c->r & 0x80u;
    c->wz.w = 0;
    c->q = 0;
    c->halted = false;
    c->int_blocked = false;
    c->prefix = 0;
    c->ld_a_ir = false;
    c->nmi_pending = false;
    c->int_line = false;     /* the field restarts at its first active line */

    m->budget = 0;
    ace_restored(m);
    return SNAP_ACE_OK;
}

const char *snap_ace_taken_on(const snap_ace_info_t *info) {
    switch (info->ramtop) {
    case 0x4000u: return "3K";
    case 0x8000u: return "19K";
    case 0xC000u: return "35K";
    default:      return "51K";
    }
}

const char *snap_ace_status_str(snap_ace_status_t st) {
    switch (st) {
    case SNAP_ACE_OK:        return "OK";
    case SNAP_ACE_IO:        return "read failed";
    case SNAP_ACE_NOT_ACE:   return "not a .ace snapshot";
    case SNAP_ACE_OTHER_RAM: return "for another machine";
    case SNAP_ACE_NO_STACK:  return "its stack is not in the file";
    case SNAP_ACE_BUSY:      return "a tape call is waiting";
    }
    return "?";
}
