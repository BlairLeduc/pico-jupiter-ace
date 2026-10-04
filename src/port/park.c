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
#include "keymatrix.h"
#include "log.h"
#include "menu.h"
#include "tapeio.h"

static volatile uint32_t g_park = PARK_NONE;
static ace_t   *s_m;
static unsigned s_page;
static bool     s_alt;

/* Core 0 to core 1: the hold may end. */
static volatile bool g_release;

volatile park_stats_t g_park_stats;

/* Core 0's, while the menu or pause has the keyboard: the UART's bytes
 * as key events for core 1, so a run over the UART can drive them.
 * ^P ^N ^B ^F are the arrows, which no ASCII byte is. */
static void uart_ui(void) {
#if PICO_ACE_UART
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        uint8_t arrow = ch == 0x10 ? PICOCALC_KEY_UP : ch == 0x0E ? PICOCALC_KEY_DOWN
                      : ch == 0x02 ? PICOCALC_KEY_LEFT : ch == 0x06 ? PICOCALC_KEY_RIGHT : 0;
        if (arrow) {
            kbd_push_uart(KEY_EV_PRESSED, arrow);
            kbd_push_uart(KEY_EV_RELEASED, arrow);
            continue;
        }
        picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
        unsigned n = keymap_picocalc_text((uint8_t)ch, ev);
        for (unsigned i = 0; i < n; i++) kbd_push_uart(ev[i].state, ev[i].code);
    }
#endif
}

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

void park_init(ace_t *m) {
    s_m = m;
}

uint32_t park(uint32_t why, unsigned page, bool alt) {
    uint32_t t0 = time_us_32();
    g_release = false;
    s_page = page;
    s_alt = alt;
    __dmb();
    g_park = why;
#if PICO_ACE_AUDIO
    static const int16_t silence[128];
#endif
    while (g_park != PARK_NONE) {
        if (why == PARK_HOLD) {
            uint8_t state, code;
            while (kbd_pop(&state, &code)) {}
            uart_release();
        } else if (why == PARK_MENU || why == PARK_PAUSE) {
            uart_ui();
        }
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

static void release(void) {
    __dmb();
    g_park = PARK_NONE;
}

/* Core 1's side of a hold: the card job on entry, again whenever the
 * card changes, and the machine handed back once core 0 has seen the
 * second GS. A step a loop, so the panel and the log keep going. */
static bool serve_hold(void) {
    static bool serving;
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
    release();
    return false;
}

bool park_serve(void) {
    uint32_t why = g_park;
    if (why == PARK_NONE) return false;
    __dmb();

    switch (why) {
    case PARK_HOLD:
        return serve_hold();
    case PARK_TAPE: {
        uint32_t us;
        tapeio_serve(s_m, &us);
        break;
    }
    case PARK_MENU:
        menu_run(s_m, s_page, s_alt);
        break;
    case PARK_PAUSE: {
        bool alt;
        int page = pause_run(&alt);
        if (page >= 0) menu_run(s_m, (unsigned)page, alt);
        break;
    }
    }
    release();
    return false;
}
