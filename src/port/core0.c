/* core0.c — core 0's loop: the guest (design.md §4.3, §11.1, §14). */

#include "core0.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "handoff.h"
#include "kbd.h"
#include "log.h"
#include "southbridge.h"

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

static void uart_keys(keymatrix_t *k, const ace_t *m) {
#if PICO_ACE_UART
    if (k->q_len + k->n_open + 2u * ACE_KEY_TEXT_EVENTS > ACE_KEY_EVENT_QUEUE) return;
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return;
    if (ch == UART_SCREEN_DUMP) {
        dump_screen(m);
        return;
    }
    picocalc_event_t ev[ACE_KEY_TEXT_EVENTS];
    unsigned n = keymap_picocalc_text((uint8_t)ch, ev);
    for (unsigned i = 0; i < n; i++) keymatrix_event(k, ev[i].state, ev[i].code);
#else
    (void)k;
    (void)m;
#endif
}

/* The keys that ask the emulator rather than the guest for something.
 * Nothing answers them until the menu (M10) and pause (M10); until then
 * they are logged and dropped, so a request does not wait for ever. */
static void requests(keymatrix_t *k) {
    if (k->menu_request)
        log_printf("  keys         : menu page %u asked for (M10)\n", (unsigned)k->menu_page);
    if (k->pause_request) log_printf("  keys         : pause asked for (M10)\n");
    if (k->reset_request) log_printf("  keys         : reset asked for (M10)\n");
    k->menu_request = k->pause_request = k->reset_request = false;
}

static void tenths(char *out, size_t n, uint32_t v10) {
    snprintf(out, n, "%lu.%lu", (unsigned long)(v10 / 10u), (unsigned long)(v10 % 10u));
}

void core0_run(ace_t *m, keymatrix_t *k) {
    const uint32_t field_t = ace_field_t(m);
    const uint32_t clk_mhz = clock_get_hz(clk_sys) / 1000000u;

    /* The schedule: field n is due at start + n fields of guest time,
     * worked out from the T-state count each time, so the rounding of a
     * field's 19,968 us never accumulates (EL §6.3). */
    uint64_t sched_us = time_us_64() + 1000u;   /* the first field on time */
    uint64_t sched_fields = 0;
    uint32_t late = 0, slips = 0;

    /* The heartbeat's window and the perf line's (§14). */
    uint64_t hb_us = time_us_64(), sec_us = hb_us;
    uint32_t hb_t = m->cpu.t, hb_insns = m->cpu.insns, hb_late = 0;
    uint32_t sec_insns = m->cpu.insns;
    uint64_t hb_run_us = 0, hb_busy_us = 0, sec_run_us = 0, sec_busy_us = 0;

    for (;;) {
        uint64_t due = sched_us + sched_fields * field_t * 1000000u / ACE_CPU_HZ;
        uint64_t now = time_us_64();
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
        uint32_t t_busy = time_us_32();

        /* Keys first, so the matrix the guest scans this field is the
         * one the events describe (§9.1). */
        uart_keys(k, m);
        uint8_t state, code;
        while (kbd_pop(&state, &code)) keymatrix_event(k, state, code);
        keymatrix_field(k, m);
        requests(k);

        uint32_t t_run = time_us_32();
        ace_run_field(m);
        uint32_t ran = time_us_32() - t_run;

        /* The field ends at the first active line, so a program that
         * redraws after the interrupt has finished (§11.1). */
        int i = pool_claim();
        if (i >= 0) {
            snapshot_fill(&g_pool.buf[i], m);
            pool_publish(i);
        }

        uint32_t busy = time_us_32() - t_busy;
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
                       "ed holes %lu, log dropped %u, battery %ld, die %ld C\n",
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
                       (long)g_c1.battery, (long)g_c1.temp_c);
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
        }
    }
}
