/* main.c — bring-up, then the two cores' loops (design.md §4.1, §15.2 M9).
 *
 * Core 0 owns the Z80, the machine and audio; core 1 owns the LCD, the
 * southbridge, the card and the log's way out (§4.3). main() sets the
 * clock, logs the banner, starts core 1 and waits while it brings up the
 * panel and reads the card's settings, then powers on the machine they
 * name (19K by default, §18 item 1) with the embedded ROM (§10.2) and
 * becomes core 0's loop.
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
#include "card.h"
#include "core0.h"
#include "core1.h"
#include "handoff.h"
#include "keymatrix.h"
#include "log.h"
#include "menu.h"
#include "park.h"
#include "pico_ace_version.h"
#include "settingsio.h"

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

    keymatrix_init(&g_keys);
    handoff_init();

    /* Core 1 reads the card while core 0 waits (§4.5, §10.6). */
    multicore_launch_core1(core1_main);
    while (!g_c1.ready) sleep_ms(1);
    __dmb();

    /* From here on core 0 logs through the ring core 1 drains. */
    log_printf("  i2c          : %lu Hz, southbridge version %ld\n",
               (unsigned long)g_c1.i2c_hz, (long)g_c1.sb_version);
    log_printf("  lcd          : spi %lu Hz\n", (unsigned long)g_c1.spi_hz);
    log_printf("  settings     : card %s, file %s%s%s; ram %s, volume %u, perf_line %s, "
               "layout %s, boot_tape %s\n",
               card_state_str(g_boot.job.state), settingsio_state_str(g_boot.cfg),
               g_boot.cfg_error[0] ? ", first problem " : "", g_boot.cfg_error,
               ace_ram_name(g_boot.settings.ram), g_boot.settings.volume,
               g_boot.settings.perf_line ? "on" : "off",
               g_boot.settings.layout[0] ? g_boot.settings.layout : "standard",
               g_boot.settings.boot_tape[0] ? g_boot.settings.boot_tape : "none");
    /* ram is the machine; volume and perf_line go to the menu's state,
     * which core 0 applies; boot_tape went into the deck as core 1 read
     * the card. layout waits for game layouts (§9.4, M15). */
    g_ui.volume = g_boot.settings.volume;
    g_ui.perf_line = g_boot.settings.perf_line;

    ace_config_t cfg;
    ace_config_default(&cfg);
    cfg.rom = ace_rom;
    cfg.ram = g_boot.settings.ram;
#ifdef PICO_ACE_BOOT_RAM
    cfg.ram = PICO_ACE_BOOT_RAM;   /* over the file's (EL §8.7) */
#endif
    if (!ace_init(&g_ace, &cfg)) {
        log_printf("  guest        : refused its configuration; not started\n");
        for (;;) sleep_ms(1000);
    }
    log_printf("  guest        : %s, %lu bytes of user RAM, %lu T a field at %lu Hz, "
               "ROM %s, hot code in SRAM to tier %u (hot.h)\n",
               ace_ram_name(cfg.ram), (unsigned long)ace_ram_bytes(cfg.ram),
               (unsigned long)ace_field_t(&g_ace),
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

    park_init(&g_ace);
    core0_run(&g_ace, &g_keys);
}
