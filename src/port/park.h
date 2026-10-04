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
 * M9 has one reason, the UART's hold, which runs the card job; the menu,
 * pause and the tape trap are M10's.
 */
#ifndef PICO_ACE_PARK_H
#define PICO_ACE_PARK_H

#include <stdbool.h>
#include <stdint.h>

#define PARK_NONE 0u
#define PARK_HOLD 1u   /* GS over the UART, until a second GS (tools/uart-hold.sh) */
/* M10: PARK_MENU, PARK_PAUSE, PARK_TAPE. */

/* GS, which no key sends: park the guest, check the card, and stay
 * parked until the next GS. */
#define UART_HOLD 0x1Du

typedef struct {
    uint32_t parks;          /* since boot                               */
    uint32_t last_us;        /* the last park, wall time                 */
    uint32_t max_us;
} park_stats_t;

extern volatile park_stats_t g_park_stats;

/* Core 0, between two fields: hand the machine to core 1 and wait for it
 * back. Returns the wall time parked, in microseconds. Keys that arrive
 * meanwhile are not the guest's and are dropped; the caller starts the
 * held set again. */
uint32_t park(uint32_t why);

/* Core 1, from its loop: the parked job, a step at a time, so the loop
 * still presents, polls the keyboard and drains the log while the guest
 * is parked. True while parked. */
bool park_serve(void);

#endif /* PICO_ACE_PARK_H */
