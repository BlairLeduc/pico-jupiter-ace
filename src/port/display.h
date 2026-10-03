/* display.h — what core 1 puts on the panel (design.md §7).
 *
 * M6 has only the test pattern; the dirty-band presenter, the status and
 * perf lines arrive with the guest (M7: display_present).
 */
#ifndef PICO_ACE_DISPLAY_H
#define PICO_ACE_DISPLAY_H

/* The bring-up pattern (design.md §15.2 M6): a 1-px white border exactly
 * on the guest's 256x192 rectangle at (32,64), with a 16x16 block in each
 * inside corner (red top-left, green top-right, blue bottom-left, yellow
 * bottom-right), so a mirrored axis or swapped R/B shows as the wrong
 * colour in the wrong corner. A 1-px grey frame on the panel's own edge
 * shows that all 320x320 are addressed. Everything else is black. */
void display_test_pattern(void);

#endif /* PICO_ACE_DISPLAY_H */
