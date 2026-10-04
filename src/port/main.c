/* main.c — bring-up, then the two cores' loops (design.md §4.1, §15.2 M7).
 *
 * Core 0 owns the Z80, the machine and audio; core 1 owns the LCD, the
 * southbridge and the log's way out (§4.3). main() sets the clock, logs
 * the banner, powers the 19K machine on with the embedded ROM (§10.2,
 * §18 item 1), starts core 1 and waits for its bring-up, then becomes
 * core 0's loop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "ace.h"
#include "ace_rom.h"
#include "audio.h"
#include "board.h"
#include "core0.h"
#include "core1.h"
#include "handoff.h"
#include "keymatrix.h"
#include "log.h"
#include "pico_ace_version.h"

/* The guest lives in .bss, not the heap: src/core/ has no allocator, and
 * keeping it static is what makes the §3.3 budget a link-time fact. */
static ace_t       g_ace;
static keymatrix_t g_keys;   /* core 0's, like g_ace */

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

    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    if (!ace_init(&g_ace, &cfg)) {
        printf("  guest        : refused its configuration; not started\n");
        for (;;) sleep_ms(1000);
    }
    keymatrix_init(&g_keys);
    handoff_init();

    multicore_launch_core1(core1_main);
    while (!g_c1.ready) sleep_ms(1);
    __dmb();

    /* From here on core 0 logs through the ring core 1 drains. */
    log_printf("  i2c          : %lu Hz, southbridge version %ld\n",
               (unsigned long)g_c1.i2c_hz, (long)g_c1.sb_version);
    log_printf("  lcd          : spi %lu Hz\n", (unsigned long)g_c1.spi_hz);
    log_printf("  guest        : %lu bytes of user RAM, %lu T a field at %lu Hz, "
               "ROM %s, hot code in SRAM to tier %u (hot.h)\n",
               (unsigned long)ace_ram_bytes(cfg.ram), (unsigned long)ace_field_t(&g_ace),
               (unsigned long)ACE_CPU_HZ, ACE_ROM_SHA1, (unsigned)PICO_ACE_RAM_TIER);

    /* Audio last in bring-up order (hardware-notes.md §10), on core 0,
     * whose IRQ the refill is, and after core 1's LCD has claimed its
     * fixed DMA channel. The beeper takes the rate the PWM really has. */
#if PICO_ACE_AUDIO
    audio_init();
    uint32_t rate_num, rate_den;
    audio_rate(&rate_num, &rate_den);
    ace_audio_set_rate(&g_ace, rate_num, rate_den);
    log_printf("  audio        : PWM GP26/GP27, %lu/%lu Hz (%lu.%02lu kHz), "
               "%u T per %u samples, ring %u slots, queue %u\n",
               (unsigned long)rate_num, (unsigned long)rate_den,
               (unsigned long)(rate_num / rate_den / 1000u),
               (unsigned long)(rate_num / rate_den % 1000u / 10u),
               (unsigned)g_ace.beeper.num, (unsigned)g_ace.beeper.den,
               (unsigned)ACE_DMA_RING_SLOTS, (unsigned)ACE_PCM_QUEUE_LEN);
#else
    log_printf("  audio        : off; pacing on the microsecond timer\n");
#endif

    core0_run(&g_ace, &g_keys);
}
