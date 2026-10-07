/* core1.h — core 1's loop: the panel, the southbridge and the log
 * (design.md §4.3).
 */
#ifndef PICO_ACE_CORE1_H
#define PICO_ACE_CORE1_H

/* Bring up the southbridge and the LCD in hardware-notes.md §10's order,
 * set g_c1.ready, then present snapshots, poll the keyboard and drain
 * core 0's log for ever. Launched with multicore_launch_core1. */
void core1_main(void);

/* Core 1: say this on the status line for a few seconds in place of what
 * it says, whether or not it is on, as a screenshot's result is said
 * (shotio.h). */
void core1_note(const char *text);

#endif /* PICO_ACE_CORE1_H */
