/* core0.c — core 0's loop: the guest (design.md §4.3, §11.1, §14). */

#include "core0.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "audio.h"
#include "card.h"
#include "handoff.h"
#include "kbd.h"
#include "log.h"
#include "park.h"
#include "settingsio.h"
#include "southbridge.h"
#include "tapeio.h"

/* The heartbeat's period (design.md §14). */
#define HEARTBEAT_US 5000000u

/* A field this far behind its deadline is not caught up: the schedule
 * starts again from now, and the slip is counted (EL §6.3). */
#define SLIP_FIELDS 3u

/* UART1's bytes typed at the guest as the PicoCalc would send them, so a
 * hardware run can be driven from the workstation that captures it
 * (§15.2 M6, tools/uart-type.sh). A byte is taken only while keymatrix
 * has room for its events and their releases, so a fast sender loses
 * characters in the UART's FIFO rather than in the replay. */
/* FS, which no key sends, asks for the screen as text in the log
 * instead (tools/uart-screen.sh), so a run driven over the UART can read
 * back what the panel shows. */
#define UART_SCREEN_DUMP 0x1Cu
/* RS opens the menu and US pauses, as Alt+M and Alt+P do; while either
 * is up the UART's bytes are its keys (park.c). */
#define UART_MENU  0x1Eu
#define UART_PAUSE 0x1Fu

#if PICO_ACE_UART
/* The guest's screen RAM as 24 lines of text: printable codes as
 * themselves, others as '.', and an inverse space (the cursor's cell
 * and the like) as '#'. Other inverse cells show as their character. */
static void dump_screen(const ace_t *m) {
    const uint8_t *scr = ace_screen(m);
    log_printf("  screen       : field %lu\n", (unsigned long)m->fields);
    for (unsigned row = 0; row < ACE_SCREEN_ROWS; row++) {
        char line[ACE_SCREEN_COLS + 1];
        for (unsigned c = 0; c < ACE_SCREEN_COLS; c++) {
            uint8_t b = scr[row * ACE_SCREEN_COLS + c], ch = b & 0x7Fu;
            char out = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : '.';
            if (b & 0x80u) out = (ch == 0x20u) ? '#' : out;
            line[c] = out;
        }
        line[ACE_SCREEN_COLS] = 0;
        log_printf("  |%s|\n", line);
    }
}
#endif

/* True when the byte was GS, a request to park (park.h). */
static bool uart_keys(keymatrix_t *k, const ace_t *m) {
#if PICO_ACE_UART
    if (k->q_len + k->n_open + 2u * ACE_KEY_TEXT_EVENTS > ACE_KEY_EVENT_QUEUE) return false;
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return false;
    if (ch == UART_SCREEN_DUMP) {
        dump_screen(m);
        return false;
    }
    if (ch == UART_HOLD) return true;
    if (ch == UART_MENU) { k->menu_request = true; k->menu_page = KM_PAGE_MAIN; return false; }
    if (ch == UART_PAUSE) { k->pause_request = true; return false; }
    picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
    unsigned n = keymap_picocalc_text((uint8_t)ch, ev);
    for (unsigned i = 0; i < n; i++) keymatrix_event(k, ev[i].state, ev[i].code);
#else
    (void)k;
    (void)m;
#endif
    return false;
}

/* The cursor the ROM draws at the input position (ROM $0282): its
 * first appearance on the bottom line is the boot reaching the prompt,
 * as test_boot has it (§15.2 M3). */
#define ACE_CURSOR 0x97u

static bool cursor_shown(const ace_t *m) {
    const uint8_t *last = ace_screen(m) + (ACE_SCREEN_ROWS - 1u) * ACE_SCREEN_COLS;
    for (unsigned c = 0; c < ACE_SCREEN_COLS; c++)
        if (last[c] == ACE_CURSOR) return true;
    return false;
}

/* What the card gave the boot, for the heartbeat: the slot now, the
 * settings file's state and first problem then (§15.2 M9), and the
 * tape's counters (M10). */
static const char *card_text(void) {
    static char text[192];
    snprintf(text, sizeof text, "card %s (%lu changes), cfg %s%s%s | tape %lu loads, "
             "%lu saves, %lu declined, %lu errors, max %lu us",
             card_present() ? "in" : "out", (unsigned long)card_changes(),
             settingsio_state_str(g_boot.cfg), g_boot.cfg_error[0] ? ": " : "",
             g_boot.cfg_error, (unsigned long)g_tape_stats.loads,
             (unsigned long)g_tape_stats.saves, (unsigned long)g_tape_stats.declined,
             (unsigned long)g_tape_stats.errors, (unsigned long)g_tape_stats.max_us);
    return text;
}

/* The keys that ask the emulator rather than the guest for something
 * (design.md §12): the menu and pause park the guest at the next
 * boundary; Alt+R resets the CPU now, RAM kept. Returns the park, or
 * PARK_NONE. */
static uint32_t requests(keymatrix_t *k, ace_t *m) {
    uint32_t why = PARK_NONE;
    if (k->reset_request) {
        ace_reset(m);
        log_printf("  keys         : reset\n");
    }
    if (k->menu_request) why = PARK_MENU;
    else if (k->pause_request) why = PARK_PAUSE;
    k->reset_request = k->pause_request = k->menu_request = false;
    return why;
}

/* What the menu changed, applied by the core that owns it (EL §2.5). */
static void apply_ui(ace_t *m) {
#if PICO_ACE_AUDIO
    audio_set_volume(g_ui.volume * 32u);
#endif
    if (g_ui.reset) {
        g_ui.reset = false;
        ace_reset(m);
        log_printf("  menu         : reset\n");
    }
    if (g_ui.power_on) {
        g_ui.power_on = false;
        ace_power_on(m);
        log_printf("  menu         : powered on again after a failed load\n");
    }
}

static void tenths(char *out, size_t n, uint32_t v10) {
    snprintf(out, n, "%lu.%lu", (unsigned long)(v10 / 10u), (unsigned long)(v10 % 10u));
}

void core0_run(ace_t *m, keymatrix_t *k) {
    const uint32_t clk_mhz = clock_get_hz(clk_sys) / 1000000u;
    uint32_t late = 0, slips = 0;   /* the timer's pacing; 0 on audio */
#if PICO_ACE_AUDIO
    /* A field's samples, drained after it and pushed to the queue. */
    static int16_t pcm[ACE_AUDIO_BUF_LEN];
    audio_stats_t au_last;
    audio_stats(&au_last, true);
#else
    const uint32_t field_t = ace_field_t(m);
    /* The schedule: field n is due at start + n fields of guest time,
     * worked out from the T-state count each time, so the rounding of a
     * field's 19,968 us never accumulates (EL §6.3). */
    uint64_t sched_us = time_us_64() + 1000u;   /* the first field on time */
    uint64_t sched_fields = 0;
#endif

    /* The heartbeat's window and the perf line's (§14). */
    uint64_t hb_us = time_us_64(), sec_us = hb_us;
    uint32_t hb_t = m->cpu.t, hb_insns = m->cpu.insns, hb_late = 0;
    uint32_t sec_insns = m->cpu.insns;
    uint64_t hb_run_us = 0, hb_busy_us = 0, sec_run_us = 0, sec_busy_us = 0;
    bool prompt = false;
    uint32_t why = PARK_NONE;
    unsigned page = 0;
    bool alt = false;
    apply_ui(m);

    for (;;) {
        uint64_t now;
#if !PICO_ACE_AUDIO
        uint64_t due = sched_us + sched_fields * field_t * 1000000u / ACE_CPU_HZ;
        now = time_us_64();
        if (now < due) {
            /* Core 0's own alarm: sleeping here interrupts nothing. */
            sleep_until(from_us_since_boot(due));
        } else if (now > due) {
            late++;
            if (now - due > (uint64_t)SLIP_FIELDS * field_t * 1000000u / ACE_CPU_HZ) {
                slips++;
                sched_us = now;
                sched_fields = 0;
            }
        }
        sched_fields++;
#endif
        /* Between two fields: the one place the guest parks (§4.5). Guest
         * time does not pass, so the schedule and the measuring windows
         * start again from now, as if the guest had just started: a
         * window across the park would count its silence as consumed
         * samples. After a hold, the menu or a pause the keys start again
         * from none held: theirs were not the guest's. A tape request
         * goes first, then whatever the keys asked for. */
        while (why != PARK_NONE || ace_tape_pending(m)) {
            uint32_t w = ace_tape_pending(m) ? PARK_TAPE : why;
            uint32_t parked = park(w, page, alt);
            if (w != PARK_TAPE) {
                keymatrix_init(k);
                why = PARK_NONE;
            }
            apply_ui(m);
            hb_us = sec_us = time_us_64();
            hb_t = m->cpu.t;
            hb_insns = sec_insns = m->cpu.insns;
            hb_late = late;
            hb_run_us = hb_busy_us = sec_run_us = sec_busy_us = 0;
#if PICO_ACE_AUDIO
            audio_stats(&au_last, true);
#else
            sched_us = hb_us + 1000u;
            sched_fields = 0;
#endif
            if (w == PARK_TAPE)
                log_printf("  park         : tape, %lu us parked\n", (unsigned long)parked);
            else
                log_printf("  park         : %lu us parked\n", (unsigned long)parked);
        }

        uint32_t t_busy = time_us_32();

        /* Keys first, so the matrix the guest scans this field is the
         * one the events describe (§9.1). */
        bool hold = uart_keys(k, m);
        uint8_t state, code;
        while (kbd_pop(&state, &code)) keymatrix_event(k, state, code);
        keymatrix_field(k, m);
        page = k->menu_page;
        alt = k->alt;
        why = requests(k, m);
        if (hold) why = PARK_HOLD;

        uint32_t t_run = time_us_32();
        ace_run_field(m);
        uint32_t ran = time_us_32() - t_run;

        /* Boot time to the prompt, from reset (§15.2 M9). */
        if (!prompt && cursor_shown(m)) {
            prompt = true;
            uint32_t at = time_us_32();
            log_printf("  boot         : prompt at field %lu, %lu.%03lu ms after reset; "
                       "core 1 ready at %lu.%03lu ms (card %s: mount %lu us, settings %lu us)\n",
                       (unsigned long)m->fields,
                       (unsigned long)(at / 1000u), (unsigned long)(at % 1000u),
                       (unsigned long)(g_boot.ready_us / 1000u),
                       (unsigned long)(g_boot.ready_us % 1000u),
                       card_state_str(g_boot.job.state), (unsigned long)g_boot.job.mount_us,
                       (unsigned long)g_boot.job.read_us);
        }

        /* The field ends at the first active line, so a program that
         * redraws after the interrupt has finished (§11.1). */
        int i = pool_claim();
        if (i >= 0) {
            snapshot_fill(&g_pool.buf[i], m);
            pool_publish(i);
        }

#if PICO_ACE_AUDIO
        size_t n = ace_audio_drain(m, pcm, ACE_AUDIO_BUF_LEN);
#endif
        uint32_t busy = time_us_32() - t_busy;
#if PICO_ACE_AUDIO
        /* Blocks while the queue is full: this is the throttle, on the
         * PWM wrap, which shares clk_sys with nothing that drifts
         * (EL §6.3). The conversion to compare words inside is not
         * counted as busy; it is ~732 short loops a field. */
        audio_push(pcm, n);
#endif
        hb_run_us += ran;
        sec_run_us += ran;
        hb_busy_us += busy;
        sec_busy_us += busy;

        now = time_us_64();
        if (now - sec_us >= 1000000u) {
            uint64_t wall = now - sec_us;
            uint32_t insns = m->cpu.insns - sec_insns;
            g_c0.busy1000 = (uint32_t)(sec_busy_us * 1000u / wall);
            g_c0.guest1000 = (uint32_t)(sec_run_us * 1000u / wall);
            g_c0.hpi10 = (uint32_t)(sec_run_us * clk_mhz * 10u / (insns + 1u));
            g_c0.late = late;
            g_c0.seconds++;
            sec_us = now;
            sec_insns = m->cpu.insns;
            sec_run_us = sec_busy_us = 0;
        }

        if (now - hb_us >= HEARTBEAT_US) {
            uint64_t wall = now - hb_us;
            uint32_t t = m->cpu.t - hb_t, insns = m->cpu.insns - hb_insns;
            /* Guest T over wall time at 3.25 MHz: 1.000 is real time. */
            uint32_t rt1000 = (uint32_t)((uint64_t)t * 1000000u / ACE_CPU_HZ * 1000u / wall);
            uint32_t busy1000 = (uint32_t)(hb_busy_us * 1000u / wall);
            uint32_t guest1000 = (uint32_t)(hb_run_us * 1000u / wall);
            /* How many times real time the guest would run unpaced. */
            uint32_t head100 = (uint32_t)((uint64_t)t * 100u * 1000000u / ACE_CPU_HZ /
                                          (hb_run_us + 1u));
            uint32_t hpi10 = (uint32_t)(hb_run_us * clk_mhz * 10u / (insns + 1u));
            uint32_t tpi100 = (uint32_t)((uint64_t)t * 100u / (insns + 1u));
            char busy_s[12], guest_s[12], hpi_s[12];
            tenths(busy_s, sizeof busy_s, busy1000);
            tenths(guest_s, sizeof guest_s, guest1000);
            tenths(hpi_s, sizeof hpi_s, hpi10);
            log_printf("  heartbeat    : %lu fields, rt %lu.%03lu, late %lu (+%lu), slips %lu | "
                       "%lu presents (%lu full, %lu dropped), last %lu us, max %lu us | "
                       "keys %lu (%lu lost), polls %lu, i2c errors %lu | "
                       "ed holes %lu, log dropped %u, battery %ld, die %ld C | "
                       "%s, parks %lu (max %lu us)\n",
                       (unsigned long)m->fields,
                       (unsigned long)(rt1000 / 1000u), (unsigned long)(rt1000 % 1000u),
                       (unsigned long)late, (unsigned long)(late - hb_late),
                       (unsigned long)slips,
                       (unsigned long)g_c1.presents, (unsigned long)g_c1.full_presents,
                       (unsigned long)g_pool.dropped,
                       (unsigned long)g_c1.last_us, (unsigned long)g_c1.max_us,
                       (unsigned long)g_c1.key_events,
                       (unsigned long)(kbd_overflows() + k->dropped),
                       (unsigned long)g_c1.polls, (unsigned long)sb_error_count(),
                       (unsigned long)m->cpu.ed_holes, log_dropped(),
                       (long)g_c1.battery, (long)g_c1.temp_c, card_text(),
                       (unsigned long)g_park_stats.parks, (unsigned long)g_park_stats.max_us);
            log_printf("  perf         : tier %u, %lu MHz, core 0 busy %s%%, guest %s%% of wall, "
                       "headroom %lu.%02lux, %s host cycles/insn, %lu.%02lu T/insn, "
                       "%lu insns\n",
                       (unsigned)PICO_ACE_RAM_TIER, (unsigned long)clk_mhz, busy_s, guest_s,
                       (unsigned long)(head100 / 100u), (unsigned long)(head100 % 100u),
                       hpi_s, (unsigned long)(tpi100 / 100u), (unsigned long)(tpi100 % 100u),
                       (unsigned long)insns);
            hb_us = now;
            hb_t = m->cpu.t;
            hb_insns = m->cpu.insns;
            hb_late = late;
            hb_run_us = hb_busy_us = 0;
#if PICO_ACE_AUDIO
            /* The consumed-sample rate against the microsecond timer is
             * the control quantity: it is the PWM wrap, measured, and it
             * must not move whatever the guest does (§14). */
            audio_stats_t au;
            audio_stats(&au, true);
            uint32_t rate = (uint32_t)((uint64_t)(au.consumed - au_last.consumed) *
                                       1000000u / (wall + 1u));
            log_printf("  audio        : %lu Hz consumed, queue %lu (low %lu), "
                       "underrun samples %lu (+%lu), late refills %lu (+%lu), "
                       "core overflow %lu, speaker edges %lu%s\n",
                       (unsigned long)rate, (unsigned long)au.level,
                       (unsigned long)au.low_water,
                       (unsigned long)au.underrun_samples,
                       (unsigned long)(au.underrun_samples - au_last.underrun_samples),
                       (unsigned long)au.late_refills,
                       (unsigned long)(au.late_refills - au_last.late_refills),
                       (unsigned long)m->beeper.overflow, (unsigned long)m->beeper.edges,
                       au.started ? "" : " (not started)");
            au_last = au;
#endif
        }
    }
}
