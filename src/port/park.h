/* park.h — the guest parked at a field boundary, the machine core 1's
 * (design.md §4.5; EL §2.5).
 *
 * Card work, the menu, pause and restart all go through here. Core 0
 * stops between two fields, names the reason in g_park, and from then
 * on the machine is core 1's until core 1 writes PARK_NONE back. Core 0
 * keeps the audio queue fed with silence meanwhile, at the rate it
 * drains, so audio neither underruns nor loses its pacing, and guest
 * time does not pass.
 *
 * Four reasons: the UART's hold, which runs the card job; a tape request
 * the CPU is stalled on (tapeio.h); the menu; and pause (menu.h). The
 * menu and pause own the keyboard while they last, so core 0 leaves the
 * key ring to core 1 for them; a hold's keys are not the guest's either,
 * and core 0 drops them; a tape's wait for the guest.
 */
#ifndef PICO_ACE_PARK_H
#define PICO_ACE_PARK_H

#include <stdbool.h>
#include <stdint.h>

#include "ace.h"

#define PARK_NONE 0u
#define PARK_HOLD  1u  /* GS over the UART, until a second GS (tools/uart-hold.sh) */
#define PARK_TAPE  2u  /* a tape block, or a recording to write (tapeio.h) */
#define PARK_MENU  3u  /* Alt+M, Alt+H or a function key (§12)          */
#define PARK_PAUSE 4u  /* Alt+P                                           */

/* GS, which no key sends: park the guest, check the card, and stay
 * parked until the next GS. */
#define UART_HOLD 0x1Du

typedef struct {
    uint32_t parks;          /* since boot                               */
    uint32_t last_us;        /* the last park, wall time                 */
    uint32_t max_us;
} park_stats_t;

extern volatile park_stats_t g_park_stats;

/* Core 0, before the first park: the machine core 1 is handed. */
void park_init(ace_t *m);

/* Core 0, between two fields: hand the machine to core 1 and wait for it
 * back. `page` and `alt` are the menu's (menu_run). Returns the wall time
 * parked, in microseconds. After a hold, the menu or a pause the caller
 * starts the held set again: their keys were not the guest's. */
uint32_t park(uint32_t why, unsigned page, bool alt);

/* Core 1, once a loop: the parked job, run when the park begins and again
 * on each card change, and the machine handed back once released. Between
 * jobs the loop still presents, polls the keyboard and drains the log; a
 * job itself is synchronous (card.h) and holds core 1 for its length, at
 * most the SD driver's bounds, inside the southbridge's 2.5 s watchdog
 * (hardware-notes.md §6.1). True while parked. */
bool park_serve(void);

#endif /* PICO_ACE_PARK_H */
