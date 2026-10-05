/* snapshot.c — our own save states (design.md §10.5; the format is in
 * snapshot.h). pico-atom's, with the Z80's and the Ace's fields. */

#include "snapshot.h"

#include <string.h>

#include "sha1.h"

static const uint8_t magic[8] = { 'P', 'A', 'C', 'E', 'S', 'N', 'A', 'P' };

/* ---- CRC-32 ---------------------------------------------------------- */

uint32_t snapshot_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- little-endian fields ---------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

/* ---- the state section ------------------------------------------------ */

enum {
    S_AF = 0, S_BC = 2, S_DE = 4, S_HL = 6, S_IX = 8, S_IY = 10, S_WZ = 12,
    S_SP = 14, S_PC = 16, S_AF_ = 18, S_BC_ = 20, S_DE_ = 22, S_HL_ = 24,
    S_I = 26, S_R = 27, S_R7 = 28, S_IFF1 = 29, S_IFF2 = 30, S_IM = 31, S_Q = 32,
    S_CPU_FLAGS = 33,             /* bits below                          */
    S_PREFIX = 34, S_INT_DATA = 35,
    S_T = 36,                     /* the T counter, 4 bytes              */
    S_RAM = 40,                   /* ace_ram_t                           */
    S_SPEAKER = 41,
    S_BUDGET = 42,                /* 4 bytes, signed                     */
    S_FIELD = 46,                 /* field_t[0..2], 12 bytes             */
    S_CRAM_READ = 58, S_OPEN_BUS = 59,
    S_ROM = 60,                   /* SHA-1 of the ROM, 20 bytes          */
    S_END = 80,                   /* the rest is reserved, written zero  */
};

_Static_assert(S_END <= SNAP_STATE_LEN, "the state section has outgrown its length");

#define F_HALTED      0x01u
#define F_INT_BLOCKED 0x02u
#define F_LD_A_IR     0x04u
#define F_INT_LINE    0x08u
#define F_NMI_PENDING 0x10u

static void rom_hash(const ace_t *m, uint8_t digest[SHA1_DIGEST_LEN]) {
    sha1(m->cfg.rom, ACE_ROM_SIZE, digest);
}

static void state_encode(const ace_t *m, uint8_t st[SNAP_STATE_LEN]) {
    memset(st, 0, SNAP_STATE_LEN);
    const z80_t *c = &m->cpu;
    put16(st + S_AF, c->af.w);  put16(st + S_BC, c->bc.w);
    put16(st + S_DE, c->de.w);  put16(st + S_HL, c->hl.w);
    put16(st + S_IX, c->ix.w);  put16(st + S_IY, c->iy.w);
    put16(st + S_WZ, c->wz.w);  put16(st + S_SP, c->sp);
    put16(st + S_PC, c->pc);
    put16(st + S_AF_, c->af_);  put16(st + S_BC_, c->bc_);
    put16(st + S_DE_, c->de_);  put16(st + S_HL_, c->hl_);
    st[S_I] = c->i; st[S_R] = c->r; st[S_R7] = c->r7;
    st[S_IFF1] = c->iff1; st[S_IFF2] = c->iff2; st[S_IM] = c->im; st[S_Q] = c->q;
    st[S_CPU_FLAGS] = (uint8_t)((c->halted ? F_HALTED : 0) | (c->int_blocked ? F_INT_BLOCKED : 0) |
                                (c->ld_a_ir ? F_LD_A_IR : 0) | (c->int_line ? F_INT_LINE : 0) |
                                (c->nmi_pending ? F_NMI_PENDING : 0));
    st[S_PREFIX] = c->prefix;
    st[S_INT_DATA] = c->int_data;
    put32(st + S_T, c->t);

    st[S_RAM] = (uint8_t)m->cfg.ram;
    st[S_SPEAKER] = m->speaker;
    put32(st + S_BUDGET, (uint32_t)m->budget);
    for (unsigned i = 0; i < 3; i++) put32(st + S_FIELD + 4u * i, m->field_t[i]);
    st[S_CRAM_READ] = m->cfg.cram_read;
    st[S_OPEN_BUS] = m->cfg.open_bus;
    rom_hash(m, st + S_ROM);
}

/* ---- the stream -------------------------------------------------------- */

/* RAM in the order it goes out, as pieces of the machine. */
typedef struct { const uint8_t *p; uint32_t n; } piece_t;

static unsigned pieces(const ace_t *m, piece_t out[4]) {
    out[0] = (piece_t){ m->vram, ACE_BLOCK_BYTES };
    out[1] = (piece_t){ m->cram, ACE_BLOCK_BYTES };
    out[2] = (piece_t){ m->uram, ACE_BLOCK_BYTES };
    uint32_t x = ace_ram_bytes(m->cfg.ram) - ACE_BLOCK_BYTES;
    out[3] = (piece_t){ m->xram, x };
    return x ? 4u : 3u;
}

snap_status_t snapshot_save(const ace_t *m, snap_write_fn write, void *ctx) {
    if (m->tape.op != TAPE_NONE) return SNAP_BUSY;

    uint8_t st[SNAP_STATE_LEN];
    state_encode(m, st);
    piece_t p[4];
    unsigned np = pieces(m, p);

    /* The CRC goes in the header, ahead of what it covers; the machine is
     * the buffer, and nothing changes it in between. */
    uint32_t crc = snapshot_crc32(0, st, sizeof st);
    for (unsigned i = 0; i < np; i++) crc = snapshot_crc32(crc, p[i].p, p[i].n);

    uint8_t hdr[SNAP_HEADER_LEN];
    memcpy(hdr, magic, sizeof magic);
    put16(hdr + 8, SNAP_VERSION);
    put16(hdr + 10, SNAP_HEADER_LEN);
    put32(hdr + 12, snap_payload_len(m->cfg.ram));
    put32(hdr + 16, crc);

    if (!write(ctx, hdr, sizeof hdr)) return SNAP_IO;
    if (!write(ctx, st, sizeof st)) return SNAP_IO;
    for (unsigned i = 0; i < np; i++)
        if (!write(ctx, p[i].p, p[i].n)) return SNAP_IO;
    return SNAP_OK;
}

static snap_status_t read_header(snap_read_fn read, void *ctx, uint32_t *len, uint32_t *crc) {
    uint8_t hdr[SNAP_HEADER_LEN];
    if (!read(ctx, hdr, sizeof hdr)) return SNAP_IO;
    if (memcmp(hdr, magic, sizeof magic) != 0) return SNAP_NOT_SNAPSHOT;
    if (get16(hdr + 8) > SNAP_VERSION) return SNAP_NEWER;
    *len = get32(hdr + 12);
    if (get16(hdr + 8) == 0 || get16(hdr + 10) != SNAP_HEADER_LEN ||
        (*len != snap_payload_len(ACE_RAM_3K) && *len != snap_payload_len(ACE_RAM_19K) &&
         *len != snap_payload_len(ACE_RAM_51K))) {
        return SNAP_NOT_SNAPSHOT;
    }
    *crc = get32(hdr + 16);
    return SNAP_OK;
}

/* Would this state resume on this machine? */
static snap_status_t compatible(const ace_t *m, const uint8_t st[SNAP_STATE_LEN], uint32_t len) {
    if (st[S_RAM] > ACE_RAM_51K || snap_payload_len((ace_ram_t)st[S_RAM]) != len)
        return SNAP_NOT_SNAPSHOT;
    if (st[S_RAM] != (uint8_t)m->cfg.ram) return SNAP_OTHER_RAM;
    for (unsigned i = 0; i < 3; i++)
        if (get32(st + S_FIELD + 4u * i) != m->field_t[i]) return SNAP_OTHER_FIELD;
    if (st[S_CRAM_READ] != m->cfg.cram_read || st[S_OPEN_BUS] != m->cfg.open_bus)
        return SNAP_OTHER_FIELD;
    if (st[S_IM] > 2u) return SNAP_NOT_SNAPSHOT;
    uint8_t rom[SHA1_DIGEST_LEN];
    rom_hash(m, rom);
    if (memcmp(rom, st + S_ROM, sizeof rom) != 0) return SNAP_OTHER_ROM;
    return SNAP_OK;
}

snap_status_t snapshot_check(const ace_t *m, snap_read_fn read, void *ctx) {
    uint32_t len, want;
    snap_status_t r = read_header(read, ctx, &len, &want);
    if (r != SNAP_OK) return r;

    uint8_t st[SNAP_STATE_LEN];
    uint8_t piece[256];
    if (!read(ctx, st, sizeof st)) return SNAP_IO;
    uint32_t crc = snapshot_crc32(0, st, sizeof st);
    for (uint32_t left = len - SNAP_STATE_LEN; left;) {
        uint32_t n = left < sizeof piece ? left : (uint32_t)sizeof piece;
        if (!read(ctx, piece, n)) return SNAP_IO;
        crc = snapshot_crc32(crc, piece, n);
        left -= n;
    }
    if (crc != want) return SNAP_CORRUPT;
    return compatible(m, st, len);
}

snap_status_t snapshot_load(ace_t *m, snap_read_fn read, void *ctx) {
    if (m->tape.op != TAPE_NONE) return SNAP_BUSY;
    uint32_t len, want;
    snap_status_t r = read_header(read, ctx, &len, &want);
    if (r != SNAP_OK) return r;
    uint8_t st[SNAP_STATE_LEN];
    if (!read(ctx, st, sizeof st)) return SNAP_IO;
    r = compatible(m, st, len);
    if (r != SNAP_OK) return r;

    /* From here the machine changes. snapshot_check has read these same
     * bytes and found them whole; a read failing now is the card going
     * away between the passes, and leaves a machine that needs a reset. */
    piece_t p[4];
    unsigned np = pieces(m, p);
    for (unsigned i = 0; i < np; i++)
        if (!read(ctx, (uint8_t *)p[i].p, p[i].n)) return SNAP_IO;

    z80_t *c = &m->cpu;
    c->af.w = get16(st + S_AF);  c->bc.w = get16(st + S_BC);
    c->de.w = get16(st + S_DE);  c->hl.w = get16(st + S_HL);
    c->ix.w = get16(st + S_IX);  c->iy.w = get16(st + S_IY);
    c->wz.w = get16(st + S_WZ);  c->sp = get16(st + S_SP);
    c->pc = get16(st + S_PC);
    c->af_ = get16(st + S_AF_);  c->bc_ = get16(st + S_BC_);
    c->de_ = get16(st + S_DE_);  c->hl_ = get16(st + S_HL_);
    c->i = st[S_I]; c->r = st[S_R]; c->r7 = st[S_R7] & 0x80u;
    c->iff1 = st[S_IFF1] ? 1u : 0u;
    c->iff2 = st[S_IFF2] ? 1u : 0u;
    c->im = st[S_IM];
    c->q = st[S_Q];
    uint8_t f = st[S_CPU_FLAGS];
    c->halted = f & F_HALTED;
    c->int_line = f & F_INT_LINE;
    c->int_blocked = f & F_INT_BLOCKED;
    c->ld_a_ir = f & F_LD_A_IR;
    c->nmi_pending = f & F_NMI_PENDING;
    c->prefix = st[S_PREFIX];
    c->int_data = st[S_INT_DATA];
    c->t = get32(st + S_T);

    m->speaker = st[S_SPEAKER] != 0;
    m->budget = (int32_t)get32(st + S_BUDGET);
    ace_restored(m);
    return SNAP_OK;
}

const char *snapshot_status_str(snap_status_t st) {
    switch (st) {
    case SNAP_OK:           return "OK";
    case SNAP_IO:           return "read or write failed";
    case SNAP_NOT_SNAPSHOT: return "not a save state";
    case SNAP_NEWER:        return "from a newer version";
    case SNAP_CORRUPT:      return "damaged (CRC)";
    case SNAP_OTHER_RAM:    return "for another machine";
    case SNAP_OTHER_FIELD:  return "another field timing";
    case SNAP_OTHER_ROM:    return "another ROM";
    case SNAP_BUSY:         return "a tape call is waiting";
    }
    return "?";
}
