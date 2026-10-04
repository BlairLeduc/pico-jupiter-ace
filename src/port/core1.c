/* core1.c — core 1's loop: the panel, the southbridge, the card and the
 * log (design.md §4.3, §4.5, §7, §9.1, §10). */

#include "core1.h"

#include <stdio.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "board.h"
#include "card.h"
#include "display.h"
#include "handoff.h"
#include "kbd.h"
#include "lcd.h"
#include "log.h"
#include "menu.h"
#include "park.h"
#include "southbridge.h"

/* Keyboard polls, as hardware-notes.md §6.1 and design.md §9.1 have them;
 * the poll also feeds the MCU's 2.5 s bus watchdog. */
#define KBD_POLL_US  33333u
/* The battery gauge and the die, for the heartbeat (design.md §14). */
#define BAT_POLL_US  5000000u
#define TEMP_POLL_US 1000000u

/* The perf line (design.md §7.4, §14): core 0's last window, and core 1's
 * longest present and dropped snapshots over its own second. */
static void draw_perf(uint32_t present_max_us, uint32_t dropped) {
    char text[96];   /* wider than the line: display_perf cuts it */
    if (!g_ui.perf_line) {
        display_perf("");   /* drawn only when its text changes */
        return;
    }
    if (g_c0.seconds == 0) {
        snprintf(text, sizeof text, "present %lu.%lums",
                 (unsigned long)(present_max_us / 1000u),
                 (unsigned long)(present_max_us % 1000u / 100u));
    } else {
        uint32_t busy = g_c0.busy1000, hpi = g_c0.hpi10;
        snprintf(text, sizeof text, "c0 %lu.%lu%% %lu.%lucy/i pr %lu.%lums dr %lu",
                 (unsigned long)(busy / 10u), (unsigned long)(busy % 10u),
                 (unsigned long)(hpi / 10u), (unsigned long)(hpi % 10u),
                 (unsigned long)(present_max_us / 1000u),
                 (unsigned long)(present_max_us % 1000u / 100u),
                 (unsigned long)dropped);
    }
    display_perf(text);
}

void core1_main(void) {
    /* 1. The southbridge first: a dead bus is the first thing to know
     *    about (hardware-notes.md §10). */
    g_c1.i2c_hz = sb_init();
    uint8_t r[2];
    if (sb_read(SB_REG_VER, r) == SB_OK) g_c1.sb_version = r[1];

    /* 2. The LCD. lcd_init sleeps through the panel's reset, which sets
     *    alarms whose IRQ is core 0's (hardware-notes.md §9.7); harmless
     *    here, because core 0 is only waiting for `ready`. Nothing after
     *    this sleeps. */
    g_c1.spi_hz = lcd_init();
    display_init();
    board_temp_init();

    /* 3. The card's settings, before the machine, because `ram` is the
     *    machine (design.md §10.6). Core 0 is waiting, so this is a job
     *    at a boundary like any parked one (§4.5); the card is optional,
     *    and without one the defaults stand. */
    card_boot(&g_boot.settings, &g_boot.job);
    g_boot.cfg = settingsio_state();
    g_boot.cfg_bytes = settingsio_bytes();
    snprintf(g_boot.cfg_error, sizeof g_boot.cfg_error, "%s", settingsio_error());
    g_boot.ready_us = time_us_32();
    menu_init(&g_boot.settings);

    __dmb();
    g_c1.ready = true;

    uint32_t now = time_us_32();
    uint32_t next_poll = now, next_bat = now, next_temp = now;
    uint32_t sec_start = now, sec_max_us = 0, sec_dropped = g_pool.dropped;
    for (;;) {
        int i = pool_take();
        if (i >= 0) {
            display_stats_t st;
            display_present(g_pool.buf[i].screen, g_pool.buf[i].charset, &st);
            pool_release(i);
            g_c1.presents++;
            if (st.full) g_c1.full_presents++;
            g_c1.last_us = st.us;
            if (st.us > g_c1.max_us) g_c1.max_us = st.us;
            if (st.us > sec_max_us) sec_max_us = st.us;
        }

        log_pump();

        /* The guest parked: its job, a step a loop. Otherwise the slot,
         * which only says what changed (card.h). */
        if (!park_serve() && card_poll())
            log_core1("  card         : %s\n", card_present() ? "in" : "out");

        now = time_us_32();
        if (now - sec_start >= 1000000u) {
            sec_start = now;
            uint32_t dropped = g_pool.dropped;
            draw_perf(sec_max_us, dropped - sec_dropped);
            sec_max_us = 0;
            sec_dropped = dropped;
        }

        /* One poll is an I2C transaction of ~4.8 ms (CLAUDE.md, M6), so
         * at most one of these runs between two presents. */
        now = time_us_32();
        if ((int32_t)(now - next_poll) >= 0) {
            next_poll = now + KBD_POLL_US;
            g_c1.key_events += kbd_poll();
            g_c1.polls++;
        } else if ((int32_t)(now - next_bat) >= 0) {
            next_bat = now + BAT_POLL_US;
            g_c1.battery = sb_read(SB_REG_BAT, r) == SB_OK ? r[1] : -1;
        } else if ((int32_t)(now - next_temp) >= 0) {
            next_temp = now + TEMP_POLL_US;
            g_c1.temp_c = board_temp_c();
        }

        /* Never sleep_us here (hardware-notes.md §9.7). */
        if (i < 0) busy_wait_us_32(20);
    }
}
