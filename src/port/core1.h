/* core1.h — core 1's loop: the panel, the southbridge and the log
 * (design.md §4.3).
 */
#ifndef PICO_ACE_CORE1_H
#define PICO_ACE_CORE1_H

/* Bring up the southbridge and the LCD in hardware-notes.md §10's order,
 * set g_c1.ready, then present snapshots, poll the keyboard and drain
 * core 0's log for ever. Launched with multicore_launch_core1. */
void core1_main(void);

#endif /* PICO_ACE_CORE1_H */
