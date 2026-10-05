/* cassette.c — tape phase 2: the signal (cassette.h, design.md §10.4).
 *
 * The half-cycles are the save routine's own, counted from the stock
 * ROM 2026-10-05 and held to its recorded signal by test_cassette. Every
 * change of level is an OUT ($FE),A at $183B, $1849, $1852, $1864 or
 * $189A, and a half-cycle is the T-states from one to the next:
 *
 *   leader  $1837-$1843: LD B,$97, DJNZ to itself, OUT, XOR 8, INC L,
 *           JR NZ, JR NZ: 2,011 T, or 2,010 when INC L reaches 0 and
 *           INC H follows. HL counts up from $E000 for a header (C = $00)
 *           and from $FC00 for data, so 8,192 or 1,024 OUTs; the first
 *           writes the level the line already has, and is no edge.
 *   sync    the last leader OUT to $1849: 601 T. $1849 to $1852: 791 T.
 *   bits    each bit is two halves, the line low and then high, MSB
 *           first: 802 T then 801 T for a 0, and 790 T more in each half
 *           for a 1 (LD B,$3D and its DJNZ at $1860). A byte's first half
 *           is longer: 799 T for the flag (from $1852 by $188A), 803 T for
 *           a data byte (the byte loop at $1872), and 805 T for the
 *           checksum (its JR Z at $1882 taken).
 *   end     the exit at $1892 drives the line low at $189A, 917 T after
 *           the checksum's last OUT.
 *
 * The flag byte goes first and is not in the checksum, which is the XOR
 * of the data; a .tap keeps the data and the checksum, and the flag is
 * the block's place (tape.h).
 */

#include "cassette.h"

#include <string.h>

#include "ace.h"
#include "hot.h"
#include "tape.h"

#define T_LEADER       2011u
#define T_LEADER_WRAP  2010u
#define T_SYNC1         601u
#define T_SYNC2         791u
#define T_FLAG_FIRST    799u
#define T_BYTE_FIRST    803u
#define T_SUM_FIRST     805u
#define T_BIT_FIRST     802u
#define T_BIT_SECOND    801u
#define T_ONE_EXTRA     790u
#define T_FINAL         917u

#define LEADER_HEADER  8192u    /* OUTs, from HL = $E000 */
#define LEADER_DATA    1024u    /* from $FC00            */

enum { PH_LEADER, PH_SYNC1, PH_SYNC2, PH_BITS, PH_FINAL, PH_GAP, PH_END };

/* ---- the player ------------------------------------------------------------ */

static void open_block(cassette_t *c) {
    c->count = 0;
    if (c->pos + 2u > c->len) {
        c->phase = PH_END;
        return;
    }
    c->n = (uint32_t)c->img[c->pos] | ((uint32_t)c->img[c->pos + 1u] << 8);
    uint32_t room = c->len - c->pos - 2u;
    c->avail = c->n < room ? c->n : room;
    c->phase = PH_LEADER;
}

static void rewind_walk(cassette_t *c) {
    c->pos = c->index = 0;
    open_block(c);
}

/* The byte the bit at `count` belongs to: the flag, then the block's. */
static uint8_t bit_byte(const cassette_t *c, uint32_t j) {
    return j == 0 ? tape_block_flag(c->index) : c->img[c->pos + 2u + j - 1u];
}

/* The next half-cycle, and move past it. */
static bool step(cassette_t *c, uint32_t *t, bool *toggles) {
    *toggles = true;
    switch (c->phase) {
    case PH_LEADER: {
        uint32_t outs = tape_block_flag(c->index) == TAPE_FLAG_HEADER ? LEADER_HEADER
                                                                       : LEADER_DATA;
        uint32_t k = ++c->count;          /* the k-th OUT, from 1 */
        *t = (k % 256u) ? T_LEADER : T_LEADER_WRAP;
        if (k + 1u >= outs) { c->phase = PH_SYNC1; c->count = 0; }
        return true;
    }
    case PH_SYNC1:
        *t = T_SYNC1;
        c->phase = PH_SYNC2;
        return true;
    case PH_SYNC2:
        *t = T_SYNC2;
        c->phase = PH_BITS;
        c->count = 0;
        return true;
    case PH_BITS: {
        /* count is the half: two a bit, eight bits a byte. */
        uint32_t half = c->count++, bit = half / 2u, j = bit / 8u, b = bit % 8u;
        bool one = (bit_byte(c, j) >> (7u - b)) & 1u;
        if (half & 1u) {
            *t = T_BIT_SECOND;
        } else if (b) {
            *t = T_BIT_FIRST;
        } else {
            *t = j == 0 ? T_FLAG_FIRST : j == c->n ? T_SUM_FIRST : T_BYTE_FIRST;
        }
        if (one) *t += T_ONE_EXTRA;
        if (c->count >= (c->avail + 1u) * 16u) c->phase = PH_FINAL;
        return true;
    }
    case PH_FINAL:
        *t = T_FINAL;
        c->phase = PH_GAP;
        return true;
    case PH_GAP:
        *t = CASSETTE_GAP_T;
        *toggles = false;
        c->pos += 2u + c->n;
        c->index++;
        open_block(c);
        return true;
    default:
        return false;
    }
}

bool cassette_walk(cassette_t *c, bool from_start, uint32_t *t, bool *toggles) {
    if (from_start) rewind_walk(c);
    return step(c, t, toggles);
}

/* Load the next change into next/toggles, or end the tape. */
static void schedule(cassette_t *c) {
    uint32_t t;
    bool toggles;
    if (!step(c, &t, &toggles)) {
        c->playing = false;
        c->ended = true;
        c->left = 0;
        return;
    }
    c->next += t;
    c->toggles = toggles;
}

void ACE_HOT1(cassette_advance)(cassette_t *c, uint32_t now) {
    while (c->playing && (int32_t)(now - c->next) >= 0) {
        if (c->toggles) {
            c->level = !c->level;
            c->edges++;
        }
        schedule(c);
    }
}

/* ---- the recorder ----------------------------------------------------------- *
 * Half-cycles are classed by length with wide margins, since only the
 * ROM's writer is decoded: a leader half is 2,010-2,011 T, the sync's
 * 601 and 791, a bit's 799-805 for a 0 and 1,589-1,595 for a 1. A bit is
 * a 1 when its two halves come to more than 2,400 T, midway between the
 * 1,603 of a 0 and the 3,183 of a 1, as the load routine at $18FC times
 * the whole cycle. */

#define REC_LEADER_MIN   1800u
#define REC_LEADER_MAX   2300u
#define REC_LEADER_RUN     64u   /* halves before a sync is believed  */
#define REC_SYNC1_MAX    1200u
#define REC_SYNC2_MIN     600u
#define REC_SYNC2_MAX    1000u
#define REC_HALF_MIN      600u
#define REC_HALF_MAX     1800u
#define REC_ONE_CYCLE    2400u

enum { RS_SEEK, RS_SYNC2, RS_BITS };

static void rec_close(cassette_t *c) {
    cassette_rec_t *r = &c->rec;
    if (r->open) {
        if (r->n == 0) {
            c->len = r->at;                 /* a flag and nothing after it */
        } else {
            r->blocks++;
            r->place++;
        }
        r->open = false;
    }
    r->state = RS_SEEK;
    r->lead = 0;
}

static void rec_byte(cassette_t *c, uint8_t b) {
    cassette_rec_t *r = &c->rec;
    if (!r->open) {
        /* The flag: the block is kept only where a .tap would read it as
         * that flag (tape.h). */
        if (b != tape_block_flag(r->place)) {
            r->errors++;
            r->state = RS_SEEK;
            r->lead = 0;
            return;
        }
        if (r->full || c->len + 2u > c->cap) { r->full = true; return; }
        r->open = true;
        r->at = c->len;
        r->n = 0;
        c->img[c->len++] = 0;
        c->img[c->len++] = 0;
        return;
    }
    if (c->len + 1u > c->cap || r->n == 0xFFFFu) { r->full = true; return; }
    c->img[c->len++] = b;
    r->n++;
    c->img[r->at] = (uint8_t)r->n;
    c->img[r->at + 1u] = (uint8_t)(r->n >> 8);
}

static void rec_half(cassette_t *c, uint32_t h) {
    cassette_rec_t *r = &c->rec;
    switch (r->state) {
    case RS_SEEK:
        if (h >= REC_LEADER_MIN && h <= REC_LEADER_MAX) {
            r->lead++;
        } else if (h < REC_SYNC1_MAX && r->lead >= REC_LEADER_RUN) {
            r->state = RS_SYNC2;
        } else {
            r->lead = 0;
        }
        break;
    case RS_SYNC2:
        if (h >= REC_SYNC2_MIN && h <= REC_SYNC2_MAX) {
            r->state = RS_BITS;
            r->half = 0;
            r->bits = 0;
            r->byte = 0;
        } else {
            r->state = RS_SEEK;
            r->lead = 0;
        }
        break;
    case RS_BITS:
        if (h < REC_HALF_MIN || h > REC_HALF_MAX) {
            rec_close(c);                   /* the signal stopped */
            break;
        }
        if (!r->half) { r->half = h; break; }
        r->byte = (uint8_t)((r->byte << 1) | (r->half + h > REC_ONE_CYCLE));
        r->half = 0;
        if (++r->bits == 8u) {
            rec_byte(c, r->byte);
            r->bits = 0;
            r->byte = 0;
        }
        break;
    }
}

void cassette_rec_edge(cassette_t *c, uint32_t now, bool level) {
    cassette_rec_t *r = &c->rec;
    if (level == r->level) return;
    rec_half(c, now - r->last);
    r->last = now;
    r->level = level;
}

/* ---- the deck in the machine ---------------------------------------------------- */

static void stop_player(ace_t *m) {
    cassette_t *c = &m->cas;
    if (!c->playing) return;
    cassette_advance(c, m->cpu.t);
    if (!c->playing) return;               /* it ended just now */
    int32_t d = (int32_t)(c->next - m->cpu.t);
    c->left = d > 0 ? (uint32_t)d : 0;
    c->playing = false;
    m->tape_in = true;                     /* no signal: the input's idle level */
}

static void stop_recorder(ace_t *m) {
    cassette_t *c = &m->cas;
    if (!c->rec.on) return;
    rec_close(c);
    c->rec.on = false;
}

/* The count of blocks in the image: the next recording's place. */
static uint32_t blocks_in(const cassette_t *c) {
    uint32_t n = 0;
    for (uint32_t p = 0; p + 2u <= c->len; n++)
        p += 2u + ((uint32_t)c->img[p] | ((uint32_t)c->img[p + 1u] << 8));
    return n;
}

static void rewind_deck(cassette_t *c) {
    rewind_walk(c);
    c->ended = false;
    c->playing = false;
    c->level = false;
    uint32_t t;
    bool toggles;
    c->next = 0;
    if (step(c, &t, &toggles)) {
        c->left = t;
        c->toggles = toggles;
    } else {
        c->left = 0;
        c->ended = true;
    }
}

void ace_cassette_insert(ace_t *m, uint8_t *img, uint32_t len, uint32_t cap) {
    ace_cassette_eject(m);
    cassette_t *c = &m->cas;
    c->img = img;
    c->len = len;
    c->cap = cap < len ? len : cap;
    c->loaded = img != NULL;
    c->rec.mark = len;
    rewind_deck(c);
    tape_hook(m);
}

void ace_cassette_eject(ace_t *m) {
    stop_player(m);
    stop_recorder(m);
    memset(&m->cas, 0, sizeof m->cas);
    m->tape_in = true;
    tape_hook(m);
}

void ace_cassette_play(ace_t *m, bool on) {
    cassette_t *c = &m->cas;
    if (!on) {
        stop_player(m);
    } else if (c->loaded && !c->playing && !c->ended && !c->rec.on) {
        c->next = m->cpu.t + c->left;
        c->playing = true;
        m->tape_in = !c->level;
    }
    tape_hook(m);
}

void ace_cassette_rewind(ace_t *m) {
    cassette_t *c = &m->cas;
    if (!c->loaded) return;
    stop_player(m);
    rewind_deck(c);
    m->tape_in = true;
    tape_hook(m);
}

void ace_cassette_record(ace_t *m, bool armed) {
    cassette_t *c = &m->cas;
    if (!armed) stop_recorder(m);
    c->rec.armed = armed && c->loaded;
    tape_hook(m);
}

bool ace_cassette_running(const ace_t *m) {
    return m->cas.playing || m->cas.rec.on;
}

bool ace_cassette_unsaved(const ace_t *m, uint32_t *from, uint32_t *to) {
    const cassette_t *c = &m->cas;
    if (!c->loaded || c->rec.on || c->len <= c->rec.mark) return false;
    *from = c->rec.mark;
    *to = c->len;
    return true;
}

void ace_cassette_saved(ace_t *m, bool keep) {
    cassette_t *c = &m->cas;
    if (!keep) {
        c->len = c->rec.mark;
        c->rec.place = blocks_in(c);
    }
    c->rec.mark = c->len;
}

/* ---- the cues (tape.c) --------------------------------------------------------- */

void cassette_cue_load(ace_t *m) {
    ace_cassette_play(m, true);
}

void cassette_cue_save(ace_t *m) {
    cassette_t *c = &m->cas;
    if (!c->rec.armed || c->rec.on) return;
    stop_player(m);
    cassette_rec_t *r = &c->rec;
    r->on = true;
    r->level = m->tape_out;
    r->last = m->cpu.t;
    r->place = blocks_in(c);
    r->state = RS_SEEK;
    r->lead = 0;
    r->open = false;
    tape_hook(m);
}

void cassette_cue_exit(ace_t *m) {
    stop_player(m);
    stop_recorder(m);
    tape_hook(m);
}

void cassette_stop_all(ace_t *m) {
    stop_player(m);
    stop_recorder(m);
    tape_hook(m);
}
