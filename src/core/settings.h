/* settings.h — the emulator's settings at power-on, and the card file
 * that changes them, /ace/pico-ace.cfg (design.md §10.6; EL §8.7).
 *
 * settings_default() is where every default lives: the machine's own
 * (ace_config_default's RAM) and the host's. The file names only what it
 * changes, one `key = value` a line.
 *
 * The parser is here, in the core, so it is tested on the host. What a
 * value names on the card — a layout, a tape — is the port's to find,
 * and it says so when it cannot.
 */
#ifndef PICO_ACE_SETTINGS_H
#define PICO_ACE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "ace.h"
#include "config.h"

/* Where a bare tape name in the file is looked for (§10.1). */
#define SETTINGS_TAPE_DIR "/ace/tapes"

typedef struct {
    ace_ram_t ram;          /* 3k, 19k or 51k (§6.2)                    */
    unsigned  volume;       /* 0-8, as the menu will show it            */
    bool      perf_line;    /* the perf line below the guest (§7.4, §14) */

    /* A layout's name, uppercase; "" is the standard map (§9.4). */
    char      layout[ACE_KEYMAP_NAME_LEN + 1];

    /* As written in the file, "" for none. The port resolves a bare name
     * against SETTINGS_TAPE_DIR. */
    char      boot_tape[ACE_PATH_MAX];
} settings_t;

void settings_default(settings_t *s);

typedef enum {
    SET_OK = 0,
    SET_SYNTAX,         /* not key = value                  */
    SET_UNKNOWN,        /* no such setting                  */
    SET_BAD_VALUE,      /* not one of the setting's values  */
    SET_DUPLICATE,      /* the setting was given twice      */
    SET_TOO_LONG,       /* a line, a name, a path, or a rewritten file */
    SET_MISMATCH,       /* a rewrite that does not read back */
} settings_status_t;

/* Apply the file's text over *s, which holds the defaults or an earlier
 * file's values. A line that is wrong changes nothing and the lines after
 * it still apply, so one typing mistake does not cost the rest of the
 * file. Returns the first line's status, and its number in *line (0 when
 * every line was good). */
settings_status_t settings_parse(settings_t *s, const char *text, size_t len,
                                 unsigned *line);
const char *settings_status_str(settings_status_t st);

/* *s written into the file's text, for the menu's save (§12). The text
 * is edited, not regenerated (EL §8.7):
 *
 *   - a key the file already gives keeps its line, its place, its
 *     indentation and its comment; only the value changes, and not even
 *     that if the value there already says the same;
 *   - a key the file does not give is appended only if the value differs
 *     from the default, so a default the user never touched keeps
 *     following the firmware's;
 *   - everything else — comments, blank lines, other keys, lines that do
 *     not parse — is copied as it stands;
 *   - the file's own line ending is kept, and appended lines use it.
 *
 * A key given twice is SET_DUPLICATE, and a result longer than
 * ACE_SETTINGS_FILE_MAX is SET_TOO_LONG. The result is parsed back
 * before it is returned, and must give *s's values again, or it is
 * SET_MISMATCH. On SET_OK, *out is the new text in a static buffer, valid
 * until the next call. No I/O. */
settings_status_t settings_rewrite(const char *text, size_t len, const settings_t *s,
                                   const char **out, size_t *out_len);

/* How the file names what is at `path`: a bare name for a file directly
 * in `dir`, otherwise the path as it is. "" stays "". */
void settings_card_name(const char *dir, const char *path, char out[ACE_PATH_MAX]);

#endif /* PICO_ACE_SETTINGS_H */
