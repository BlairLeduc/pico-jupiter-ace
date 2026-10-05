/* test_settings.c — the defaults, /ace/pico-ace.cfg's parser, and the
 * save that edits it (settings.h, design.md §10.6; EL §8.7). Needs no
 * ROM. Brought from pico-atom with its keys changed for the Ace's.
 */

#include <string.h>

#include "settings.h"
#include "test_util.h"

static settings_status_t parse(settings_t *s, const char *text, unsigned *line) {
    settings_default(s);
    return settings_parse(s, text, strlen(text), line);
}

int main(void) {
    settings_t s, d;
    unsigned line;

    /* ---- the defaults are the machine's and the host's ---------------- */
    settings_default(&d);
    {
        ace_config_t cfg;
        ace_config_default(&cfg);
        CHECK(d.ram == cfg.ram && d.ram == ACE_RAM_19K, "the 19K machine (§18 item 1)");
        CHECK(d.volume == 8u && !d.perf_line, "full volume, no perf line");
        CHECK(d.fast_tape, "fast tape (§10.3)");
        CHECK(!d.layout[0] && !d.boot_tape[0], "the standard map, no tape");
    }

    /* ---- an empty file, and one of comments, change nothing ----------- */
    CHECK(parse(&s, "", &line) == SET_OK && line == 0, "empty");
    CHECK(memcmp(&s, &d, sizeof s) == 0, "empty changes nothing");
    CHECK(parse(&s, "# nothing\n\n   # indented\r\n", &line) == SET_OK && line == 0, "comments");
    CHECK(memcmp(&s, &d, sizeof s) == 0, "comments change nothing");

    /* ---- every setting ------------------------------------------------ */
    {
        const char *all =
            "# every key\r\n"
            "RAM       = 3K\r\n"
            "volume    = 0\r\n"
            "layout    = cursor games\r\n"
            "boot_tape = /ace/tapes/My Tape.tap\r\n"
            "Perf_Line = On\r\n"
            "fast_tape = off\r\n";
        CHECK(parse(&s, all, &line) == SET_OK && line == 0, "every setting: line %u", line);
        CHECK(s.ram == ACE_RAM_3K && s.volume == 0u && s.perf_line, "ram, volume, perf_line");
        CHECK(!s.fast_tape, "fast_tape");
        CHECK(strcmp(s.layout, "CURSOR GAMES") == 0, "layout uppercased, spaces kept: %s",
              s.layout);
        CHECK(strcmp(s.boot_tape, "/ace/tapes/My Tape.tap") == 0, "boot_tape: %s", s.boot_tape);
    }
    CHECK(parse(&s, "ram = 51k\n", &line) == SET_OK && s.ram == ACE_RAM_51K, "51K");
    CHECK(parse(&s, "ram = 3k\nram2 = x\n", &line) == SET_UNKNOWN && s.ram == ACE_RAM_3K,
          "3K, with a stranger after it");
    CHECK(parse(&s, "layout = Standard\n", &line) == SET_OK && !s.layout[0], "standard");
    CHECK(parse(&s, "boot_tape =   \n", &line) == SET_OK && !s.boot_tape[0], "empty means none");

    /* ---- comments after a value --------------------------------------- */
    CHECK(parse(&s, "ram = 3k   # the stock machine\r\n"
                    "volume = 3\t# 0-8\n"
                    "boot_tape =          # none\n", &line) == SET_OK && line == 0,
          "trailing comments: line %u", line);
    CHECK(s.ram == ACE_RAM_3K && s.volume == 3u && !s.boot_tape[0], "trailing comments stripped");
    CHECK(parse(&s, "boot_tape = side#2.tap # the second\n", &line) == SET_OK &&
          strcmp(s.boot_tape, "side#2.tap") == 0, "a # inside a path is kept: %s", s.boot_tape);
    CHECK(parse(&s, "ram # = 3k\n", &line) == SET_SYNTAX && line == 1,
          "a comment before the = leaves no value");

    /* ---- a bad line changes nothing, and the rest still apply --------- */
    {
        static const struct { const char *text; settings_status_t st; } bad[] = {
            { "ram 3k\n",              SET_SYNTAX },
            { "= 3k\n",                SET_SYNTAX },
            { "ram =\n",               SET_SYNTAX },
            { "layout =\n",            SET_SYNTAX },
            { "memory = 3k\n",         SET_UNKNOWN },
            { "ram = 3\n",             SET_BAD_VALUE },
            { "ram = 16k\n",           SET_BAD_VALUE },
            { "ram = 3kb\n",           SET_BAD_VALUE },
            { "ram = 35k\n",           SET_BAD_VALUE },
            { "volume = 9\n",          SET_BAD_VALUE },
            { "volume = -1\n",         SET_BAD_VALUE },
            { "volume = 99999999999\n", SET_BAD_VALUE },
            { "perf_line = yes\n",     SET_BAD_VALUE },
            { "field_hz = 60\n",       SET_UNKNOWN },   /* a constant (§16) */
            { "layout = A NAME LONGER THAN 16\n", SET_TOO_LONG },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            settings_status_t st = parse(&s, bad[i].text, &line);
            CHECK(st == bad[i].st && line == 1, "%s-> %s, line %u", bad[i].text,
                  settings_status_str(st), line);
            CHECK(memcmp(&s, &d, sizeof s) == 0, "%schanged something", bad[i].text);
        }
    }
    {
        const char *mixed = "volume = 3\nvolume = 5\nram = 4k\nperf_line = on\n";
        CHECK(parse(&s, mixed, &line) == SET_DUPLICATE && line == 2, "first problem: line %u", line);
        CHECK(s.volume == 3u && s.ram == ACE_RAM_19K && s.perf_line,
              "good lines apply around the bad ones");
    }
    {
        /* A value refused does not count as given, so a later line may
         * still set it. */
        CHECK(parse(&s, "ram = 2k\nram = 3k\n", &line) == SET_BAD_VALUE && line == 1, "retry");
        CHECK(s.ram == ACE_RAM_3K, "the good line applies");
    }
    {
        char text[ACE_PATH_MAX + 16];
        memcpy(text, "boot_tape = ", 12);
        memset(text + 12, 'a', ACE_PATH_MAX);
        text[12 + ACE_PATH_MAX] = 0;
        CHECK(parse(&s, text, &line) == SET_TOO_LONG && !s.boot_tape[0],
              "path longer than ACE_PATH_MAX");

        char huge[400];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = 0;
        CHECK(parse(&s, huge, &line) == SET_TOO_LONG && line == 1, "line too long");
    }
    {
        const char nul[] = "ram = 3k\0\nvolume = 2\n";
        settings_default(&s);
        CHECK(settings_parse(&s, nul, sizeof nul - 1, &line) == SET_SYNTAX && line == 1, "NUL");
        CHECK(s.ram == ACE_RAM_19K && s.volume == 2u, "the NUL line is refused, the next applies");
    }

    /* ---- the menu's save edits the file (§12; EL §8.7) ---------------- */
    const char *out;
    size_t out_len;
#define REWRITE(text, set) settings_rewrite(text, strlen(text), set, &out, &out_len)
#define IS(want) (out_len == strlen(want) && memcmp(out, want, out_len) == 0)

    /* Nothing to say: an empty file, or none, stays empty. */
    CHECK(REWRITE("", &d) == SET_OK && out_len == 0, "defaults into an empty file: %zu", out_len);

    /* The user's file, CRLF, with comments, a bad line and a stranger. */
    {
        const char *file =
            "# my PicoCalc\r\n"
            "  ram = 3K          # the stock machine\r\n"
            "volume = 11\r\n"
            "boot_tape =      # none\r\n"
            "frobnicate = yes\r\n"
            "\r\n"
            "layout = cursor games\r\n";
        settings_default(&s);
        CHECK(settings_parse(&s, file, strlen(file), &line) == SET_BAD_VALUE && line == 3, "setup");
        /* What the menu changed: the machine, the volume and the tape. */
        s.ram = ACE_RAM_51K;
        s.volume = 5u;
        strcpy(s.boot_tape, "NEW01.tap");
        CHECK(REWRITE(file, &s) == SET_OK, "rewrites");
        const char *want =
            "# my PicoCalc\r\n"
            "  ram = 51k         # the stock machine\r\n"
            "volume = 5\r\n"
            "boot_tape = NEW01.tap # none\r\n"
            "frobnicate = yes\r\n"
            "\r\n"
            "layout = cursor games\r\n";
        CHECK(IS(want), "the user's file, edited:\n%.*s\nwanted:\n%s", (int)out_len, out, want);

        /* Saving again changes nothing more. */
        static char again[ACE_SETTINGS_FILE_MAX];
        memcpy(again, out, out_len);
        size_t again_len = out_len;
        CHECK(settings_rewrite(again, again_len, &s, &out, &out_len) == SET_OK &&
              out_len == again_len && memcmp(out, again, out_len) == 0, "a second save is the same");
    }

    /* The layout the menu chose (§9.4), in place beside its comment, or
     * added to a file that had none; a second save is the same. */
    {
        const char *file = "layout = cursor   # the arrows as 5-8\nram = 19k\n";
        settings_default(&s);
        CHECK(settings_parse(&s, file, strlen(file), &line) == SET_OK, "layout setup");
        CHECK(strcmp(s.layout, "CURSOR") == 0, "layout read as %s", s.layout);
        strcpy(s.layout, "QAOP");
        CHECK(REWRITE(file, &s) == SET_OK &&
                  IS("layout = QAOP     # the arrows as 5-8\nram = 19k\n"),
              "a new layout in place:\n%.*s", (int)out_len, out);
        static char again[ACE_SETTINGS_FILE_MAX];
        memcpy(again, out, out_len);
        size_t again_len = out_len;
        CHECK(settings_rewrite(again, again_len, &s, &out, &out_len) == SET_OK &&
                  out_len == again_len && memcmp(out, again, out_len) == 0,
              "a second save of the layout is the same");

        s.layout[0] = 0;
        CHECK(REWRITE(file, &s) == SET_OK &&
                  IS("layout = standard # the arrows as 5-8\nram = 19k\n"),
              "the standard map in place:\n%.*s", (int)out_len, out);

        const char *none = "ram = 19k # mine\n";
        settings_default(&s);
        strcpy(s.layout, "CURSOR");
        CHECK(REWRITE(none, &s) == SET_OK && out_len > strlen(none) &&
                  memcmp(out, none, strlen(none)) == 0 && strstr(out, "layout = CURSOR\n"),
              "a layout the file lacked is added after the user's lines:\n%.*s", (int)out_len,
              out);
    }

    /* A refused value is replaced where it stands, even by the default,
     * so a save clears the problem; when another line gives the key a
     * good value, which writing it would make given twice, the refused
     * line becomes a comment instead. */
    {
        settings_default(&s);
        CHECK(REWRITE("ram = 3k\nvolume = 9\n", &s) == SET_OK &&
              IS("ram = 19k\nvolume = 8\n"), "refused, replaced: %.*s", (int)out_len, out);
        settings_default(&s);
        CHECK(settings_parse(&s, out, out_len, &line) == SET_OK && line == 0,
              "and read back without a problem");
        settings_default(&s);
        s.volume = 3u;
        CHECK(REWRITE("ram = 3k\nvolume = 9\nvolume = 2\n", &s) == SET_OK &&
              IS("ram = 19k\n# volume = 9\nvolume = 3\n"),
              "refused, but another line is the key's: %.*s", (int)out_len, out);
        settings_default(&s);
        CHECK(settings_parse(&s, out, out_len, &line) == SET_OK && line == 0 && s.volume == 3u,
              "and read back without a problem");
    }

    /* A value that already says the same stays as the user wrote it,
     * however it is spelt. */
    {
        const char *file = "RAM = 3K\nlayout = cursor games\nboot_tape = /ace/tapes/Chuck.TAP\n";
        CHECK(parse(&s, file, &line) == SET_OK, "setup");
        strcpy(s.boot_tape, "chuck.tap");
        CHECK(REWRITE(file, &s) == SET_OK && IS(file), "unchanged: %.*s", (int)out_len, out);
    }

    /* Appended lines: only what differs from the default, with the
     * file's line ending, after a last line that had none. */
    {
        settings_default(&s);
        s.ram = ACE_RAM_3K;
        s.perf_line = true;
        s.fast_tape = false;
        strcpy(s.layout, "GAMES");
        CHECK(REWRITE("# mine\r\nvolume = 8", &s) == SET_OK &&
              IS("# mine\r\nvolume = 8\r\nram = 3k\r\nlayout = GAMES\r\nperf_line = on\r\n"
                 "fast_tape = off\r\n"),
              "appended: %.*s", (int)out_len, out);
        s.ram = ACE_RAM_19K;
        CHECK(REWRITE("ram = 3k\n", &s) == SET_OK && strncmp(out, "ram = 19k\n", 10) == 0,
              "back to the default is written where the file has it");
        s.layout[0] = 0;
        CHECK(REWRITE("layout = games\n", &s) == SET_OK && strstr(out, "layout = standard\n"),
              "the standard map is written by name");
    }

    /* Three columns: a value put in, one changed and one taken out all
     * leave the comments where they were. */
    {
        const char *file =
            "ram       = 3k         # 3k, 19k or 51k\n"
            "boot_tape =            # a file in /ace/tapes/\n"
            "layout    = games      # a layout\n"
            "volume\t= 8\t# 0-8\n";
        CHECK(parse(&s, file, &line) == SET_OK, "setup");
        s.ram = ACE_RAM_51K;
        strcpy(s.boot_tape, "TAPE01.tap");
        s.layout[0] = 0;
        s.volume = 3u;
        const char *want =
            "ram       = 51k        # 3k, 19k or 51k\n"
            "boot_tape = TAPE01.tap # a file in /ace/tapes/\n"
            "layout    = standard   # a layout\n"
            "volume\t= 3\t# 0-8\n";
        CHECK(REWRITE(file, &s) == SET_OK && IS(want), "columns kept:\n%.*s\nwanted:\n%s",
              (int)out_len, out, want);
        strcpy(s.boot_tape, "A-MUCH-LONGER-NAME.tap");
        CHECK(REWRITE(file, &s) == SET_OK &&
              strstr(out, "boot_tape = A-MUCH-LONGER-NAME.tap # a file"),
              "a value too long for the column pushes its comment one space on:\n%.*s",
              (int)out_len, out);
    }

    /* Refusals leave nothing to write. */
    settings_default(&s);
    s.volume = 2u;
    CHECK(REWRITE("volume = 3\nram = 3k\nvolume = 4\n", &s) == SET_DUPLICATE,
          "a key given twice refuses the save");
    CHECK(REWRITE("volume = 11\nvolume = 4\n", &s) == SET_OK &&
          IS("# volume = 11\nvolume = 2\n"),
          "a refused line beside the key's own becomes a comment: %.*s", (int)out_len, out);
    {
        static char big[ACE_SETTINGS_FILE_MAX + 1];
        memset(big, '#', ACE_SETTINGS_FILE_MAX - 4u);
        big[ACE_SETTINGS_FILE_MAX - 4u] = '\n';
        big[ACE_SETTINGS_FILE_MAX - 3u] = 0;
        CHECK(REWRITE(big, &s) == SET_TOO_LONG, "a result too long for the file refuses");
    }
    strcpy(s.boot_tape, "/ace/tapes/a #1.tap");
    CHECK(REWRITE("", &s) == SET_MISMATCH, "a name the file cannot hold does not read back");

    /* What the port saves a path as. */
    {
        char n[ACE_PATH_MAX];
        settings_card_name(SETTINGS_TAPE_DIR, "/ace/tapes/x.tap", n);
        CHECK(strcmp(n, "x.tap") == 0, "bare: %s", n);
        settings_card_name(SETTINGS_TAPE_DIR, "/ace/tapes/sub/x.tap", n);
        CHECK(strcmp(n, "/ace/tapes/sub/x.tap") == 0, "a subfolder keeps its path: %s", n);
        settings_card_name(SETTINGS_TAPE_DIR, "/ace/snaps/x.tap", n);
        CHECK(strcmp(n, "/ace/snaps/x.tap") == 0, "another folder keeps its path: %s", n);
        settings_card_name(SETTINGS_TAPE_DIR, "", n);
        CHECK(n[0] == 0, "none is none");
    }

    TEST_DONE();
}
