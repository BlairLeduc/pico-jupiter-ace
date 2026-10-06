/* status.c — what the status and perf lines say (status.h, design.md §12). */

#include "status.h"

#include <stdio.h>
#include <string.h>

#include "ace.h"

void ace_status(const ace_t *m, ace_status_t *st) {
    const cassette_t *c = &m->cas;
    memset(st, 0, sizeof *st);
    if (!c->loaded) return;

    if (c->rec.on || c->rec.armed) st->deck = c->rec.full ? STATUS_DECK_FULL : STATUS_DECK_REC;
    else if (c->playing) st->deck = STATUS_DECK_PLAY;
    else if (c->ended) st->deck = STATUS_DECK_END;
    else st->deck = STATUS_DECK_STOP;
    uint32_t pct = st->deck == STATUS_DECK_REC || st->deck == STATUS_DECK_FULL
                       ? (c->cap ? (uint32_t)((uint64_t)c->len * 100u / c->cap) : 100u)
                       : (c->len ? (uint32_t)((uint64_t)c->pos * 100u / c->len) : 100u);
    st->percent = (uint8_t)(pct > 100u ? 100u : pct);
    st->errors = (uint16_t)(c->rec.errors > 0xFFFFu ? 0xFFFFu : c->rec.errors);
}

/* A path's file name without its folder or extension, at most `max`
 * characters, in its own case: the Ace has lower case. */
static void short_name(const char *path, char *out, size_t max) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (n > max) n = max;
    memcpy(out, b, n);
    out[n] = 0;
}

/* As much of `s` as fits in the line from *at. */
static void put(char *out, size_t *at, const char *s) {
    for (; *s && *at < ACE_TEXT_COLS; s++) out[(*at)++] = *s;
}

static const char *deck_word(uint8_t deck) {
    switch (deck) {
    case STATUS_DECK_STOP: return "Stop";
    case STATUS_DECK_PLAY: return "Play";
    case STATUS_DECK_END:  return "End";
    case STATUS_DECK_REC:  return "Rec";
    case STATUS_DECK_FULL: return "Full";
    }
    return "Tape";
}

void status_format(const ace_status_t *st, const char *tape, char out[ACE_TEXT_COLS + 1]) {
    enum { W = ACE_TEXT_COLS };
    memset(out, ' ', W);
    out[W] = 0;
    if (st->deck == STATUS_DECK_IDLE && !(tape && tape[0])) return;

    /* The word, how far, the name, then turbo and errors, with the name
     * given whatever room is left. */
    char head[16], tail[24] = "";
    bool pct = st->deck == STATUS_DECK_STOP || st->deck == STATUS_DECK_PLAY ||
               st->deck == STATUS_DECK_REC;
    snprintf(head, sizeof head, pct ? "%s %u%%" : "%s", deck_word(st->deck),
             (unsigned)st->percent);
    size_t tn = 0;
    if (st->turbo10)
        tn += (size_t)snprintf(tail + tn, sizeof tail - tn, " %u.%ux", st->turbo10 / 10u,
                               st->turbo10 % 10u);
    if (st->errors) snprintf(tail + tn, sizeof tail - tn, " %u bad", (unsigned)st->errors);

    size_t fixed = strlen(head) + strlen(tail);
    size_t room = fixed + 1u < W ? W - fixed - 1u : 0u;
    char name[W + 1];
    short_name(tape ? tape : "", name, room);
    size_t at = 0;
    put(out, &at, head);
    if (name[0]) put(out, &at, " ");
    put(out, &at, name);
    put(out, &at, tail);
}

/* ---- the perf line and Paused --------------------------------------------- */

static unsigned clamp(uint32_t v, unsigned max) {
    return v > max ? max : (unsigned)v;
}

void status_perf_format(const perf_line_t *p, char out[ACE_TEXT_COLS + 1]) {
    enum { W = ACE_TEXT_COLS };
    unsigned pct = clamp((p->busy1000 + 5u) / 10u, 100u);
    unsigned head = clamp(p->head100, 999u * 100u + 99u);
    unsigned ms10 = clamp((p->present_us + 50u) / 100u, 9999u);
    /* Two spaces between the groups while they fit, one when the counts
     * have grown, so the last figure is never the one that goes. */
    char text[64];
    int n = 0;
    for (unsigned gap = 2; gap >= 1; gap--) {
        const char *sp = gap == 2 ? "  " : " ";
        n = snprintf(text, sizeof text, "C0 %u%% %u.%02ux%sLCD %u.%ums%sDrop %u%sUR %u %u",
                     pct, head / 100u, head % 100u, sp, ms10 / 10u, ms10 % 10u, sp,
                     clamp(p->dropped, 999u), sp, clamp(p->underruns, 99999u),
                     clamp(p->late, 9999u));
        if (n >= 0 && (size_t)n <= W) break;
    }
    size_t len = n < 0 ? 0u : (size_t)n < W ? (size_t)n : W;
    memset(out, ' ', W);
    memcpy(out, text, len);
    out[W] = 0;
}

void status_paused_format(char out[ACE_TEXT_COLS + 1]) {
    enum { W = ACE_TEXT_COLS };
    static const char text[] = "Paused: any key resumes";
    memset(out, ' ', W);
    memcpy(out, text, sizeof text - 1u);
    out[W] = 0;
}
