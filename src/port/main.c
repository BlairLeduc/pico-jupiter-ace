/* main.c — M6's firmware: board bring-up, no guest yet (design.md §15.2 M6).
 *
 * Core 1 owns the southbridge and the LCD (§4.3). It brings both up,
 * times the blits and an I2C transaction, draws the test pattern, then
 * polls the keyboard at 30 Hz and drains core 0's log ring. Core 0 logs
 * every key event from the keyboard and from the UART, and a heartbeat
 * once a second. M7 puts the guest on core 0 and the presenter on
 * core 1.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "board.h"
#include "config.h"
#include "display.h"
#include "kbd.h"
#include "keymatrix.h"
#include "lcd.h"
#include "log.h"
#include "pico_ace_version.h"
#include "southbridge.h"

/* Keyboard polls, as hardware-notes.md §6.1 and design.md §9.1 have them;
 * the poll also feeds the MCU's 2.5 s bus watchdog. */
#define KBD_POLL_US 33333u

/* Blits timed per size at bring-up. */
#define BLIT_RUNS 8u

typedef struct {
    uint32_t min_us, max_us;
} span_t;

/* Core 1's results and counters, read by core 0. Each is a 32-bit word
 * with one writer, so a torn read is not possible. */
static volatile struct {
    bool     ready;          /* bring-up finished, results below valid */
    uint32_t i2c_hz, spi_hz;
    int32_t  sb_version;     /* SB_REG_VER's byte, -1 if unread        */
    span_t   blit_full;      /* 320x320 on the wire                    */
    span_t   blit_guest;     /* 256x192 on the wire                    */
    uint32_t polls;
    uint32_t key_events;
    uint32_t i2c_last_us;    /* one FIFO read of an empty FIFO         */
    uint32_t i2c_min_us, i2c_max_us;
    int32_t  temp_c;         /* the die, INT32_MIN until read          */
} g_c1 = { .sb_version = -1, .i2c_min_us = UINT32_MAX, .temp_c = INT32_MIN };

static uint16_t s_line[ACE_LINEBUF_COUNT][ACE_LINEBUF_PIXELS];

/* ---- core 1 ------------------------------------------------------------ */

/* A w x h blit from the ping-pong line buffers, rows alternating, timed.
 * The buffers are filled once, so this is the wire and the DMA alone:
 * M7 measures the renderer that fills them. */
static span_t time_blit(unsigned x, unsigned y, unsigned w, unsigned h) {
    span_t s = { UINT32_MAX, 0 };
    for (unsigned run = 0; run < BLIT_RUNS; run++) {
        uint32_t t0 = time_us_32();
        lcd_blit_begin(x, y, w, h);
        for (unsigned row = 0; row < h; row++) lcd_blit_row(s_line[row & 1u], w);
        lcd_blit_end();
        uint32_t us = time_us_32() - t0;
        if (us < s.min_us) s.min_us = us;
        if (us > s.max_us) s.max_us = us;
    }
    return s;
}

static void core1_main(void) {
    g_c1.i2c_hz = sb_init();
    uint8_t r[2];
    if (sb_read(SB_REG_VER, r) == SB_OK) g_c1.sb_version = r[1];

    /* lcd_init sleeps through the panel's reset. That sets alarms whose
     * IRQ is core 0's (hardware-notes.md §9.7), harmless here because
     * core 0 is only waiting for `ready`; nothing after this sleeps. */
    g_c1.spi_hz = lcd_init();

    /* A grey ramp in one buffer, its inverse in the other, so a blit
     * that drops or repeats rows would show. */
    for (unsigned i = 0; i < ACE_LINEBUF_PIXELS; i++) {
        uint16_t v = (uint16_t)(i * 31u / (ACE_LINEBUF_PIXELS - 1u));
        uint16_t grey = (uint16_t)(v << 11 | (v * 2u) << 5 | v);
        s_line[0][i] = grey;
        s_line[1][i] = (uint16_t)~grey;
    }
    g_c1.blit_full = time_blit(0, 0, ACE_PANEL_W, ACE_PANEL_H);
    g_c1.blit_guest = time_blit(ACE_SCREEN_X, ACE_SCREEN_Y, ACE_SCREEN_W, ACE_SCREEN_H);
    display_test_pattern();
    board_temp_init();

    __dmb();
    g_c1.ready = true;

    uint32_t next_poll = time_us_32();
    uint32_t next_temp = next_poll;
    for (;;) {
        uint32_t now = time_us_32();
        if ((int32_t)(now - next_poll) >= 0) {
            next_poll += KBD_POLL_US;
            uint32_t errors = sb_error_count();
            uint32_t t0 = time_us_32();
            unsigned n = kbd_poll();
            uint32_t us = time_us_32() - t0;
            g_c1.polls++;
            g_c1.key_events += n;
            /* An empty FIFO is one transaction: a register write and a
             * two-byte read (hardware-notes.md §6.1). */
            if (n == 0 && sb_error_count() == errors) {
                g_c1.i2c_last_us = us;
                if (us < g_c1.i2c_min_us) g_c1.i2c_min_us = us;
                if (us > g_c1.i2c_max_us) g_c1.i2c_max_us = us;
            }
        }
        if ((int32_t)(now - next_temp) >= 0) {
            next_temp += 1000000u;
            g_c1.temp_c = board_temp_c();
        }
        log_pump();
        /* Never sleep_us here (hardware-notes.md §9.7). */
        busy_wait_us_32(200);
    }
}

/* ---- core 0 ------------------------------------------------------------ */

static const char *state_name(uint8_t state) {
    switch (state) {
    case KEY_EV_PRESSED:  return "press  ";
    case KEY_EV_HELD:     return "held   ";
    case KEY_EV_RELEASED: return "release";
    default:              return "?      ";
    }
}

static void log_key(const char *src, uint8_t state, uint8_t code) {
    if (code >= 0x20u && code < 0x7Fu)
        log_printf("  key          : %-4s %s 0x%02X '%c'\n", src, state_name(state), code, code);
    else
        log_printf("  key          : %-4s %s 0x%02X\n", src, state_name(state), code);
}

/* UART1's bytes as the PicoCalc's events, logged beside the keyboard's
 * so the two can be compared (§15.2 M6). M7 feeds them to keymatrix. */
static uint32_t uart_keys(void) {
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return 0;
    picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
    unsigned n = keymap_picocalc_text((uint8_t)ch, ev);
    if (n == 0) log_printf("  key          : uart byte 0x%02X sends no key\n", (uint8_t)ch);
    for (unsigned i = 0; i < n; i++) log_key("uart", ev[i].state, ev[i].code);
    return n;
}

static void heartbeat(unsigned secs, uint32_t uart_events) {
    log_printf("  heartbeat    : %u s, %lu polls, %lu kbd + %lu uart events, "
               "i2c %lu errors, last %lu us (%lu-%lu), ring overflows %lu, "
               "log dropped %u, die %ld C\n",
               secs, (unsigned long)g_c1.polls, (unsigned long)g_c1.key_events,
               (unsigned long)uart_events, (unsigned long)sb_error_count(),
               (unsigned long)g_c1.i2c_last_us, (unsigned long)g_c1.i2c_min_us,
               (unsigned long)g_c1.i2c_max_us, (unsigned long)kbd_overflows(), log_dropped(),
               (long)g_c1.temp_c);
}

int main(void) {
    /* The clock first, so the UART's divider is worked out from the
     * clock that stays. */
    bool clocks_ok = board_init_clocks();
    stdio_init_all();
    board_info_t board;
    board_identify(&board);

    /* A startup banner plus consecutive heartbeats is more useful boot
     * evidence than a single line (hardware-notes.md §2.7). Core 1 is not
     * running yet, so printf may block. */
    board_log_banner(&board);
    printf("  firmware     : %s\n", PICO_ACE_VERSION);
    if (!clocks_ok) {
        printf("  WARNING: clk_sys is not at 150 MHz; SPI and audio rates will "
               "not be the ones this build assumes\n");
    }

    multicore_launch_core1(core1_main);
    while (!g_c1.ready) sleep_ms(1);
    __dmb();

    /* From here on core 0 logs through the ring core 1 drains. */
    log_printf("  i2c          : %lu Hz, southbridge version %ld\n",
               (unsigned long)g_c1.i2c_hz, (long)g_c1.sb_version);
    log_printf("  lcd          : spi %lu Hz\n", (unsigned long)g_c1.spi_hz);
    log_printf("  blit 320x320 : %lu-%lu us over %u runs\n",
               (unsigned long)g_c1.blit_full.min_us, (unsigned long)g_c1.blit_full.max_us,
               BLIT_RUNS);
    log_printf("  blit 256x192 : %lu-%lu us over %u runs\n",
               (unsigned long)g_c1.blit_guest.min_us, (unsigned long)g_c1.blit_guest.max_us,
               BLIT_RUNS);

    uint32_t uart_events = 0;
    uint32_t start = time_us_32(), next_beat = start + 1000000u;
    for (unsigned secs = 1;;) {
        uint8_t state, code;
        while (kbd_pop(&state, &code)) log_key("kbd", state, code);
        uart_events += uart_keys();

        if ((int32_t)(time_us_32() - next_beat) >= 0) {
            next_beat += 1000000u;
            heartbeat(secs++, uart_events);
        }
        /* No guest yet: core 0 may sleep (hardware-notes.md §9.7 is
         * about core 1's sleeps landing on core 0). */
        sleep_us(500);
    }
}
