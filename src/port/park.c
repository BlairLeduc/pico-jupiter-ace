/* park.c — the guest parked at a field boundary (park.h, design.md §4.5).
 *
 * pico-atom's park(), in its own file: g_park is the hand-off word,
 * written non-zero by core 0 and back to PARK_NONE by core 1, with a
 * barrier on each side of every write so the machine changes hands with
 * everything either core wrote to it.
 */

#include "park.h"

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "audio.h"
#include "card.h"
#include "kbd.h"
#include "log.h"

static volatile uint32_t g_park = PARK_NONE;

/* Core 0 to core 1: the hold may end. */
static volatile bool g_release;

volatile park_stats_t g_park_stats;

/* Core 0's: the UART's second GS releases a hold. Other bytes are not
 * the guest's while it is parked, and are dropped. */
static void uart_release(void) {
#if PICO_ACE_UART
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == UART_HOLD) g_release = true;
    }
#endif
}

uint32_t park(uint32_t why) {
    uint32_t t0 = time_us_32();
    g_release = false;
    __dmb();
    g_park = why;
#if PICO_ACE_AUDIO
    static const int16_t silence[128];
#endif
    while (g_park != PARK_NONE) {
        uint8_t state, code;
        while (kbd_pop(&state, &code)) {}
        uart_release();
#if PICO_ACE_AUDIO
        /* Blocks while the queue is full, so this runs at the rate the
         * PWM drains it (EL §6.3). */
        audio_push(silence, sizeof silence / sizeof silence[0]);
#else
        /* Core 0's own alarm: sleeping here interrupts nothing. */
        sleep_us(100);
#endif
    }
    __dmb();
    uint32_t us = time_us_32() - t0;
    g_park_stats.last_us = us;
    if (us > g_park_stats.max_us) g_park_stats.max_us = us;
    g_park_stats.parks++;
    return us;
}

/* Core 1's side of a hold: the card job on entry, again whenever the
 * card changes, and the machine handed back once core 0 has seen the
 * second GS. */
bool park_serve(void) {
    static bool serving;
    if (g_park == PARK_NONE) return false;
    __dmb();

    settings_t s;
    card_job_t j;
    if (!serving) {
        serving = true;
        log_core1("  park         : held; GS again to resume\n");
        card_check(&s, &j);
    } else if (card_poll()) {
        log_core1("  card         : %s while parked\n", card_present() ? "in" : "out");
        card_check(&s, &j);
    }
    if (!g_release) return true;

    serving = false;
    log_core1("  park         : resumed\n");
    __dmb();
    g_park = PARK_NONE;
    return false;
}
