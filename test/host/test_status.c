/* test_status.c — the status and perf lines' text, and what core 0
 * hands over for them (status.h, design.md §12). Needs no ROM run.
 */

#include <string.h>

#include "ace.h"
#include "ace_rom.h"
#include "status.h"
#include "test_util.h"

static char line[ACE_TEXT_COLS + 1];

static const char *fmt(const ace_status_t *st, const char *tape) {
    status_format(st, tape, line);
    return line;
}

/* The line with its padding taken off, for comparing. */
static const char *trimmed(void) {
    static char t[ACE_TEXT_COLS + 1];
    memcpy(t, line, sizeof t);
    for (int i = ACE_TEXT_COLS - 1; i >= 0 && t[i] == ' '; i--) t[i] = 0;
    return t;
}

static ace_t m;

int main(void) {
    ace_status_t st;

    /* ---- the text ---------------------------------------------------- */
    memset(&st, 0, sizeof st);
    fmt(&st, "");
    CHECK(strlen(line) == ACE_TEXT_COLS && !trimmed()[0], "nothing to say is a blank line");
    fmt(&st, "/ace/tapes/tut-tut.tap");
    CHECK(strcmp(trimmed(), "Tape tut-tut") == 0, "the trap's tape: [%s]", line);

    st.deck = STATUS_DECK_PLAY;
    st.percent = 42;
    fmt(&st, "/ace/tapes/tut-tut.tap");
    CHECK(strcmp(trimmed(), "Play 42% tut-tut") == 0, "playing: [%s]", line);
    st.turbo10 = 32;
    fmt(&st, "/ace/tapes/tut-tut.tap");
    CHECK(strcmp(trimmed(), "Play 42% tut-tut 3.2x") == 0, "turbo: [%s]", line);

    memset(&st, 0, sizeof st);
    st.deck = STATUS_DECK_END;
    st.percent = 100;
    fmt(&st, "x.tap");
    CHECK(strcmp(trimmed(), "End x") == 0, "at the end: [%s]", line);
    st.deck = STATUS_DECK_REC;
    st.percent = 3;
    st.errors = 2;
    fmt(&st, "/ace/tapes/TAPE01.tap");
    CHECK(strcmp(trimmed(), "Rec 3% TAPE01 2 bad") == 0, "recording: [%s]", line);
    st.deck = STATUS_DECK_FULL;
    st.errors = 0;
    fmt(&st, "/ace/tapes/TAPE01.tap");
    CHECK(strcmp(trimmed(), "Full TAPE01") == 0, "out of room: [%s]", line);

    memset(&st, 0, sizeof st);
    st.deck = STATUS_DECK_STOP;
    st.turbo10 = 31;
    fmt(&st, "/ace/tapes/A very long tape name indeed, longer than the line.tap");
    CHECK(strlen(line) == ACE_TEXT_COLS, "still one line: [%s]", line);
    CHECK(strncmp(line, "Stop 0% A very long", 19) == 0, "the name shortened: [%s]", line);
    CHECK(strcmp(line + ACE_TEXT_COLS - 5, " 3.1x") == 0, "the turbo kept: [%s]", line);

    /* ---- what core 0 hands over --------------------------------------- */
    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    ace_init(&m, &cfg);
    ace_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_IDLE && st.turbo10 == 0, "an empty cassette");

    /* One 3-byte block: a length of 2, a byte and its XOR. */
    static uint8_t img[64] = { 2, 0, 0x55, 0x55 };
    ace_cassette_insert(&m, img, 4, sizeof img);
    ace_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_STOP && st.percent == 0, "a tape in, stopped: %u %u%%",
          st.deck, st.percent);
    ace_cassette_play(&m, true);
    ace_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_PLAY, "playing: %u", st.deck);
    ace_cassette_play(&m, false);
    ace_cassette_record(&m, true);
    ace_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_REC && st.percent == 4u * 100u / sizeof img,
          "recording, and how full: %u %u%%", st.deck, st.percent);
    ace_cassette_record(&m, false);
    ace_cassette_eject(&m);
    ace_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_IDLE, "ejected: %u", st.deck);

    /* ---- the perf line ------------------------------------------------ */
    {
        perf_line_t p = { .busy1000 = 214, .head100 = 465, .present_us = 11860 };
        status_perf_format(&p, line);
        CHECK(strlen(line) == ACE_TEXT_COLS, "the perf line fills the width");
        CHECK(strcmp(trimmed(), "C0 21% 4.65x  LCD 11.9ms  Drop 0  UR 0 0") == 0,
              "the idle board: '%s'", trimmed());
        p = (perf_line_t){ .busy1000 = 868, .head100 = 115, .present_us = 16749,
                           .dropped = 3, .underruns = 611, .late = 2 };
        status_perf_format(&p, line);
        CHECK(strcmp(trimmed(), "C0 87% 1.15x LCD 16.7ms Drop 3 UR 611 2") == 0,
              "a busy second closes up to fit: '%s'", trimmed());
        p = (perf_line_t){ .busy1000 = 5000, .head100 = 0xFFFFFFFFu, .present_us = 0xFFFFFFFFu,
                           .dropped = 0xFFFFFFFFu, .underruns = 0xFFFFFFFFu,
                           .late = 0xFFFFFFFFu };
        status_perf_format(&p, line);
        CHECK(strlen(line) == ACE_TEXT_COLS && strncmp(line, "C0 100% 999.99x", 15) == 0,
              "clamped: '%s'", line);
    }

    /* ---- Paused --------------------------------------------------------- */
    status_paused_format(line);
    CHECK(strlen(line) == ACE_TEXT_COLS && strncmp(trimmed(), "Paused", 6) == 0,
          "the paused line: '%s'", trimmed());

    TEST_DONE();
}
