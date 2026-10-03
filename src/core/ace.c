/* ace.c — the guest machine: page table, even port, field (design.md §6, §11). */

#include "ace.h"

#include <string.h>

#include "hot.h"

void ace_config_default(ace_config_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->ram = ACE_RAM_19K;                    /* §18 item 1 */
    cfg->rom = NULL;

    /* MAME's jupace (read 2026-10-03): a 416-pixel line at 6.5 MHz is
     * 208 T, 312 lines; the display drawn from line 56 for 192 lines; INT
     * asserted at line 248 and cleared at 256. Not yet checked against
     * the schematic (§16). */
    cfg->line_t      = 208;
    cfg->field_lines = 312;
    cfg->int_line    = 248;
    cfg->int_t       = 8 * 208;
    cfg->active_line = 56;

    /* Neither value is settled (§16). The ROM's RAM sizing needs only an
     * unpopulated read that is not $FC (§6.2); $FF is MAME's port idle
     * level and the usual pulled-up bus. */
    cfg->cram_read = 0xFFu;
    cfg->open_bus  = 0xFFu;
}

uint32_t ace_ram_bytes(ace_ram_t ram) {
    switch (ram) {
    case ACE_RAM_3K:  return ACE_BLOCK_BYTES;
    case ACE_RAM_51K: return ACE_BLOCK_BYTES + ACE_XRAM_MAX;
    case ACE_RAM_19K:
    default:          return ACE_BLOCK_BYTES + 16384u;
    }
}

/* ---- The bus's slow path (§6.1) --------------------------------------- */

/* Only character RAM and unpopulated pages have no read pointer. */
static uint8_t ACE_HOT1(mem_read)(void *ctx, uint16_t addr) {
    const ace_t *m = ctx;
    if ((addr & 0xF800u) == ACE_CRAM_BASE) return m->cfg.cram_read;
    return m->cfg.open_bus;
}

/* ROM and unpopulated pages: the write goes nowhere. */
static void ACE_HOT1(mem_write)(void *ctx, uint16_t addr, uint8_t v) {
    (void)ctx;
    (void)addr;
    (void)v;
}

/* Any access to an even port moves the speaker: IN one way, OUT the
 * other (§2.3, §8). Every one, including the INs that only read keys. */
static inline void speaker_to(ace_t *m, bool level) {
    if (m->speaker != level) {
        m->speaker = level;
        m->speaker_edges++;
        /* M8: the beeper takes the edge, stamped at the instruction's start. */
    }
}

/* Decoded on A0 alone, by mask (§6.5). An even read ANDs together every
 * half-row whose address line is low; keys and D5's tape level are
 * active low, and D6-D7 read high (§16). */
static uint8_t ACE_HOT1(io_read)(void *ctx, uint16_t port) {
    ace_t *m = ctx;
    if (port & 1u) return m->cfg.open_bus;

    uint8_t v = 0xFFu;
    unsigned rows = (unsigned)(~port >> 8) & 0xFFu;
    for (int r = 0; rows; r++, rows >>= 1)
        if (rows & 1u) v &= (uint8_t)~m->keys[r];
    if (!m->tape_in) v &= (uint8_t)~0x20u;

    speaker_to(m, false);
    return v;
}

static void ACE_HOT1(io_write)(void *ctx, uint16_t port, uint8_t v) {
    ace_t *m = ctx;
    (void)v;               /* the data byte is not decoded (§2.3, §16) */
    if (port & 1u) return;
    speaker_to(m, true);
}

/* ---- Power-on --------------------------------------------------------- */

static void map(ace_t *m, unsigned first, unsigned last,
                const uint8_t *read, uint8_t *write, unsigned mirror_pages) {
    for (unsigned p = first; p <= last; p++) {
        unsigned off = (mirror_pages ? (p - first) % mirror_pages : p - first) * ACE_PAGE_SIZE;
        m->page[p].read  = read  ? read + off  : NULL;
        m->page[p].write = write ? write + off : NULL;
    }
}

static void build_pages(ace_t *m) {
    const unsigned block = ACE_BLOCK_BYTES / ACE_PAGE_SIZE;   /* 4 pages */

    map(m, 0x00u, 0xFFu, NULL, NULL, 0);                       /* open bus */
    map(m, ACE_ROM_BASE >> 8, (ACE_ROM_BASE + ACE_ROM_SIZE - 1u) >> 8,
        m->cfg.rom, NULL, 0);
    map(m, ACE_VRAM_BASE >> 8, (ACE_CRAM_BASE >> 8) - 1u, m->vram, m->vram, block);
    map(m, ACE_CRAM_BASE >> 8, (ACE_URAM_BASE >> 8) - 1u, NULL, m->cram, block);
    map(m, ACE_URAM_BASE >> 8, (ACE_XRAM_BASE >> 8) - 1u, m->uram, m->uram, block);

    uint32_t x = ace_ram_bytes(m->cfg.ram) - ACE_BLOCK_BYTES;
    if (x)
        map(m, ACE_XRAM_BASE >> 8, (ACE_XRAM_BASE + x - 1u) >> 8, m->xram, m->xram, 0);
}

static void connect_bus(ace_t *m) {
    m->cpu.bus.page      = m->page;
    m->cpu.bus.ctx       = m;
    m->cpu.bus.mem_read  = mem_read;
    m->cpu.bus.mem_write = mem_write;
    m->cpu.bus.io_read   = io_read;
    m->cpu.bus.io_write  = io_write;
}

bool ace_init(ace_t *m, const ace_config_t *cfg) {
    if (!cfg->rom || cfg->line_t == 0 || cfg->int_t == 0) return false;
    uint32_t lines = cfg->field_lines;
    if (cfg->int_line >= lines || cfg->active_line >= lines) return false;

    /* The field runs from the first active line round to the next. */
    uint32_t field = lines * cfg->line_t;
    uint32_t to_int = ((cfg->int_line + lines - cfg->active_line) % lines) * cfg->line_t;
    if (to_int == 0 || to_int + cfg->int_t > field) return false;

    /* Zero-filled RAM (§6.3). Ace Forth has no random-number word, so
     * there is no seed for zeroed RAM to leave stuck: the manual's RND
     * keeps its own and seeds it from FRAMES ($3C2B), which the
     * interrupt counts up (§16). */
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    m->field_t[0] = to_int;
    m->field_t[1] = cfg->int_t;
    m->field_t[2] = field - to_int - cfg->int_t;

    m->tape_in = true;
    build_pages(m);
    connect_bus(m);
    z80_reset(&m->cpu);
    return true;
}

void ace_reset(ace_t *m) {
    z80_reset(&m->cpu);
}

void ace_copy(ace_t *dst, const ace_t *src) {
    if (dst == src) return;
    memcpy(dst, src, sizeof *dst);

    /* Pointers into src move to the same place in dst; the ROM is
     * outside both and stays. */
    const uint8_t *lo = (const uint8_t *)src, *hi = lo + sizeof *src;
    uint8_t *base = (uint8_t *)dst;
    for (unsigned p = 0; p < ACE_PAGE_COUNT; p++) {
        const uint8_t *r = src->page[p].read;
        const uint8_t *w = src->page[p].write;
        if (r >= lo && r < hi) dst->page[p].read  = base + (r - lo);
        if (w >= lo && w < hi) dst->page[p].write = base + (w - lo);
    }
    connect_bus(dst);
}

/* ---- Running ---------------------------------------------------------- */

uint32_t ACE_HOT2(ace_run)(ace_t *m, uint32_t t_states) {
    return z80_run(&m->cpu, t_states);
}

/* Spend the budget. Negative is debt from the last instruction's
 * overshoot, paid off by running nothing. */
static uint32_t run_budget(ace_t *m, uint32_t part) {
    m->budget += (int32_t)part;
    if (m->budget <= 0) return 0;
    uint32_t done = ace_run(m, (uint32_t)m->budget);
    m->budget -= (int32_t)done;
    return done;
}

uint32_t ace_run_field(ace_t *m) {
    uint32_t done = run_budget(m, m->field_t[0]);
    z80_set_int(&m->cpu, true);
    done += run_budget(m, m->field_t[1]);
    z80_set_int(&m->cpu, false);
    done += run_budget(m, m->field_t[2]);
    m->fields++;
    return done;
}

/* ---- Inputs and inspection -------------------------------------------- */

void ace_key_set(ace_t *m, int row, int col, bool down) {
    if (row < 0 || row > 7 || col < 0 || col > 4) return;
    uint8_t bit = (uint8_t)(1u << col);
    if (down) m->keys[row] |= bit;
    else      m->keys[row] = (uint8_t)(m->keys[row] & ~bit);
}

uint8_t ace_peek(const ace_t *m, uint16_t addr) {
    const uint8_t *p = m->page[addr >> 8].read;
    if (p) return p[addr & 0xFFu];
    return mem_read((void *)m, addr);
}
