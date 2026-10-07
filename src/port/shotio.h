/* shotio.h — screenshots to the card: the whole panel as
 * /ace/shots/SHOTnnnn.bmp (design.md §12).
 *
 * Core 1 only, with the guest parked (park.h), like all card work: from
 * the guest, F6 parks it for the shot alone; in the menu or pause it is
 * parked already. The image is what the panel shows, regenerated row by
 * row from what was last sent to it (display_panel_row), so a menu page
 * is taken as readily as the guest's screen. The file is written as
 * SHOTnnnn.new and renamed once whole, as the other writers do.
 */
#ifndef PICO_ACE_SHOTIO_H
#define PICO_ACE_SHOTIO_H

#include <stdbool.h>

#define SHOTIO_DIR "/ace/shots"

/* The panel into the first number after the highest SHOTnnnn.bmp there.
 * `mounted` says the caller has the card mounted (the menu); otherwise
 * this mounts it and unmounts it after. Returns what to tell the user,
 * the file saved or why not, short enough for the menu's status row
 * with a space before it. */
const char *shotio_take(bool mounted);

#endif /* PICO_ACE_SHOTIO_H */
