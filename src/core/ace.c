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

    cfg->tape_traps = true;
    cfg->halt_skip  = true;
}

uint32_t ace_ram_bytes(ace_ram_t ram) {
    switch (ram) {
    case ACE_RAM_3K:  return ACE_BLOCK_BYTES;
    case ACE_RAM_51K: return ACE_BLOCK_BYTES + ACE_XRAM_MAX;
    case ACE_RAM_19K:
    default:          return ACE_BLOCK_BYTES + ACE_XRAM_19K;
    }
}

const char *ace_ram_name(ace_ram_t ram) {
    switch (ram) {
    case ACE_RAM_3K:  return "3K";
    case ACE_RAM_51K: return "51K";
    case ACE_RAM_19K:
    default:          return "19K";
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
 * other (§2.3, §8). Every one, including the INs that only read keys.
 * The edge is stamped at the start of the accessing instruction, not at
 * its I/O cycle: the offset is the same for every edge a loop makes, so
 * pitch is exact and only the phase is early (EL §6.1). The Z80 adds an
 * instruction's T-states after its bus accesses, so cpu.t during an IN
 * or OUT is that start; test_audio's box filter holds it to that. */
static inline void speaker_to(ace_t *m, bool level) {
    if (m->speaker != level) {
        m->speaker = level;
        beeper_set_level(&m->beeper, m->cpu.t, level);
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

    /* The field runs from the first active line round to the next. In 64
     * bits, so that nothing wraps before it is checked; the parts become a
     * signed budget (run_budget), so the whole must fit in an int32_t. */
    uint64_t field = (uint64_t)lines * cfg->line_t;
    uint64_t to_int = (uint64_t)((cfg->int_line + lines - cfg->active_line) % lines) * cfg->line_t;
    if (field > INT32_MAX || to_int == 0 || to_int + cfg->int_t > field) return false;

    /* Zero-filled RAM (§6.3). Ace Forth has no random-number word, so
     * there is no seed for zeroed RAM to leave stuck: the manual's RND
     * keeps its own and seeds it from FRAMES ($3C2B), which the
     * interrupt counts up (§16). */
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    m->field_t[0] = (uint32_t)to_int;
    m->field_t[1] = cfg->int_t;
    m->field_t[2] = (uint32_t)(field - to_int - cfg->int_t);

    m->tape_in = true;
    build_pages(m);
    connect_bus(m);
    tape_init(m);
    z80_reset(&m->cpu);
    m->cpu.halt_skip = cfg->halt_skip;
    beeper_init(&m->beeper, m->cpu.t, m->speaker, ACE_CPU_HZ,
                ACE_AUDIO_RATE_NUM, ACE_AUDIO_RATE_DEN);
    return true;
}

void ace_power_on(ace_t *m) {
    ace_config_t cfg = m->cfg;
    uint32_t num = m->beeper.num, den = m->beeper.den;
    bool dc_block = m->beeper.dc_block;
    if (!ace_init(m, &cfg)) return;      /* it was running with this cfg */
    /* T-states a sample = num / den, so a clock of num and a rate of den
     * over 1 gives the same fraction (beeper_init). */
    beeper_init(&m->beeper, m->cpu.t, m->speaker, num, den, 1u);
    m->beeper.dc_block = dc_block;
}

void ace_reset(ace_t *m) {
    z80_reset(&m->cpu);
    /* A request goes with the program that made it. */
    m->tape.op = TAPE_NONE;
    m->tape.pass = false;
}

void ace_restored(ace_t *m) {
    m->tape.op = TAPE_NONE;
    m->tape.pass = false;
    m->tape.begun = false;
    memset(m->keys, 0, sizeof m->keys);
    beeper_restart(&m->beeper, m->cpu.t, m->speaker);
}

/* The page table is a function of cfg, so the copy's is rebuilt over its
 * own buffers rather than relocated from the original's pointers. */
void ace_copy(ace_t *dst, const ace_t *src) {
    if (dst == src) return;
    memcpy(dst, src, sizeof *dst);
    build_pages(dst);
    connect_bus(dst);
}

/* ---- Running ---------------------------------------------------------- */

uint32_t ACE_HOT2(ace_run)(ace_t *m, uint32_t t_states) {
    uint32_t done = z80_run(&m->cpu, t_states);
    /* Close off every sample that ended inside this run, so a drain
     * after it sees them all (§8). Once per call, not per instruction. */
    beeper_advance(&m->beeper, m->cpu.t);
    return done;
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

/* ---- Audio (§8) -------------------------------------------------------- */

void ace_audio_set_rate(ace_t *m, uint32_t rate_num, uint32_t rate_den) {
    beeper_t *b = &m->beeper;
    beeper_advance(b, m->cpu.t);
    bool dc_block = b->dc_block;
    beeper_init(b, m->cpu.t, m->speaker, ACE_CPU_HZ, rate_num, rate_den);
    b->dc_block = dc_block;
}

size_t ace_audio_drain(ace_t *m, int16_t *dst, size_t max) {
    return beeper_drain(&m->beeper, dst, max);
}

/* ---- Inputs and inspection -------------------------------------------- */

void ace_key_set(ace_t *m, int row, int col, bool down) {
    if (row < 0 || row >= (int)ACE_KEY_ROWS || col < 0 || col >= (int)ACE_KEY_COLS) return;
    uint8_t bit = (uint8_t)(1u << col);
    if (down) m->keys[row] |= bit;
    else      m->keys[row] = (uint8_t)(m->keys[row] & ~bit);
}

uint8_t ace_peek(const ace_t *m, uint16_t addr) {
    const uint8_t *p = m->page[addr >> 8].read;
    if (p) return p[addr & 0xFFu];
    return mem_read((void *)m, addr);
}

void ace_poke(ace_t *m, uint16_t addr, uint8_t v) {
    uint8_t *p = m->page[addr >> 8].write;
    if (p) p[addr & 0xFFu] = v;
    else mem_write(m, addr, v);
}
