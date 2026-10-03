/* main.c — M0's firmware: the banner, then a heartbeat and the status LED
 * (design.md §15.2). One core and no guest yet; core 0's and core 1's
 * loops get their own files when there is something to run (§4.1). */

#include <stdbool.h>
#include <stdio.h>

#include "pico/status_led.h"
#include "pico/stdlib.h"

#include "board.h"
#include "pico_ace_version.h"

static board_info_t g_board;

int main(void) {
    /* The clock first, so the UART's divider is worked out from the
     * clock that stays. */
    bool clocks_ok = board_init_clocks();
    stdio_init_all();
    board_temp_init();
    board_identify(&g_board);

    /* A startup banner plus consecutive heartbeats is more useful boot
     * evidence than a single line (hardware-notes.md §2.7). */
    board_log_banner(&g_board);
    printf("  firmware     : %s\n", PICO_ACE_VERSION);
    if (!clocks_ok) {
        printf("  WARNING: clk_sys is not at 150 MHz; SPI and audio rates will "
               "not be the ones this build assumes\n");
    }

    /* A pico2 image drives GP25, which is the LED on a Pico 2 and the
     * radio's CS on a W board, whose LED it cannot reach
     * (hardware-notes.md §1.1, §2.5). The heartbeat is the evidence that
     * holds on every board. */
    bool led = status_led_init();
    printf("  status LED   : %s\n",
           led ? "GP25 (lights on a Pico 2 only)" : "none in this build");

    /* M0 runs one core, with no guest and no deadline, so a sleep and a
     * blocking printf here cost nothing (hardware-notes.md §9.7). */
    for (unsigned beat = 0;; beat++) {
        if (led) status_led_set_state(true);
        sleep_ms(500);
        if (led) status_led_set_state(false);
        sleep_ms(500);
        printf("  heartbeat    : %u, die %d C\n", beat, board_temp_c());
    }
}
