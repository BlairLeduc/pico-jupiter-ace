/* settingsio.h — the settings file on the card, /ace/pico-ace.cfg
 * (design.md §10.6; EL §8.7).
 *
 * Core 1, with the card mounted (storage.h) and core 0 waiting or
 * parked. There is no file on a new card, and that is not a problem:
 * everything keeps its default (settings.h).
 *
 * The first problem is kept for the heartbeat, and for the menu's status
 * row once there is one (M10): a line the parser refused, or a file that
 * could not be read.
 *
 * The menu's save writes the file through a temporary file and a rename
 * (EL §8.6); a load that finds only the temporary file, a save cut off
 * between the two, takes it.
 */
#ifndef PICO_ACE_SETTINGSIO_H
#define PICO_ACE_SETTINGSIO_H

#include <stdint.h>

#include "settings.h"

#define SETTINGSIO_DIR  "/ace"
#define SETTINGSIO_PATH "/ace/pico-ace.cfg"
#define SETTINGSIO_TEMP "/ace/pico-ace.new"

typedef enum {
    SETTINGSIO_NO_CARD,     /* not looked for: no card, or none mounted */
    SETTINGSIO_NO_FILE,     /* the card has no file: the defaults       */
    SETTINGSIO_READ,        /* read, every line or with a problem kept  */
    SETTINGSIO_UNREADABLE,  /* there, but too big or not readable       */
} settingsio_state_t;

/* The file's settings over the defaults, into *out. The card must be
 * mounted. */
void settingsio_load(settings_t *out);

/* The defaults, with nothing read, for a boot without a card. */
void settingsio_none(settings_t *out);

settingsio_state_t settingsio_state(void);
const char *settingsio_state_str(settingsio_state_t st);

/* The file's size, read by the last load; 0 without one. */
uint32_t settingsio_bytes(void);

/* "" when there was no problem; else, e.g., "line 3: no such setting". */
const char *settingsio_error(void);

/* A problem found with a value after the file was read, such as a
 * layout the card does not have: logged, and kept if it is the first. */
void settingsio_fail(const char *what, const char *why);

/* *s into the file, edited in place (settings_rewrite) and written
 * through SETTINGSIO_TEMP and a rename. The card must be mounted. NULL,
 * or why not, for the menu's status row. */
const char *settingsio_save(const settings_t *s);

#endif /* PICO_ACE_SETTINGSIO_H */
