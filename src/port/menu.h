/* menu.h — the emulator's menu and pause (design.md §12; EL §10).
 *
 * Core 1, with the guest parked (park.h): the machine is core 1's for as
 * long as either lasts. The keyboard is core 1's too: it polls the
 * southbridge and takes the events itself, since core 0, their usual
 * consumer, is parked. What the menu changes for core 0 (the volume, a
 * reset) goes through g_ui, which core 0 applies when it has the machine
 * back (EL §2.5).
 *
 * The menu is a text page through the guest's row generator (textpage.h),
 * and closing it invalidates the presenter, so the next snapshot is drawn
 * whole. Its pages and rows are pico-atom's (menu.c says where they
 * differ). Its status row names the first problem: a layout a file
 * chose, the tape's last word, then the settings file's first bad line.
 */
#ifndef PICO_ACE_MENU_H
#define PICO_ACE_MENU_H

#include <stdbool.h>

#include "ace.h"
#include "settings.h"

/* What the settings file said at boot, kept for the save: what the
 * menu does not set, and a layout a file chose, is saved from here
 * (EL §8.7). Core 1, at boot. */
void menu_init(const settings_t *file);

/* Run the menu until it is closed. `page` is KM_PAGE_MAIN or the page a
 * function key or Alt+H asked for, which closing then returns from to
 * the guest. `alt` is whether Alt was down when it was asked for, so
 * that Alt+M closes it only as a chord. */
void menu_run(ace_t *m, unsigned page, bool alt);

/* Pause: the guest's last frame stays, the backlight is dimmed, and the
 * status line says Paused, whether or not it is on. Any key resumes and
 * is not typed; a modifier alone does not, nor does the pause chord's
 * own repeat. Alt+M, Alt+H and the function keys go to the menu instead: returns -1 to resume, or the
 * page, with whether Alt was down in *alt. */
int pause_run(bool *alt);

#endif /* PICO_ACE_MENU_H */
