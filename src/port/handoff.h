/* handoff.h — what the two cores share (design.md §4.3, §4.4).
 *
 * The snapshot pool, with every transition under one SIO spinlock
 * (snappool.h says why the lock is the port's), and the counters each
 * core keeps for the other's heartbeat and perf line. Each counter is a
 * 32-bit word with one writer, so a torn read is not possible.
 */
#ifndef PICO_ACE_HANDOFF_H
#define PICO_ACE_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "card.h"
#include "keymatrix.h"
#include "settings.h"
#include "settingsio.h"
#include "snappool.h"

extern snappool_t g_pool;

void handoff_init(void);

/* snappool.h's transitions, each under the lock. */
int  pool_claim(void);
void pool_publish(int i);
int  pool_take(void);
void pool_release(int i);

/* Core 1's, read by core 0. */
typedef struct {
    bool     ready;          /* bring-up finished, the fields below valid */
    uint32_t i2c_hz, spi_hz;
    int32_t  sb_version;     /* SB_REG_VER's byte, -1 if unread           */
    uint32_t presents;
    uint32_t full_presents;
    uint32_t last_us;        /* the last present, wall time               */
    uint32_t max_us;         /* the longest since boot                    */
    uint32_t polls;
    uint32_t key_events;
    int32_t  battery;        /* SB_REG_BAT's byte, -1 until read          */
    int32_t  temp_c;         /* the die, INT32_MIN until read             */
} core1_stats_t;

extern volatile core1_stats_t g_c1;

/* What core 1 found on the card at boot (design.md §10.6), written
 * before g_c1.ready and only read after it, so it needs no lock. The
 * heartbeat names the file's first problem from here: what a later
 * parked check finds is logged, not applied (M10). */
typedef struct {
    settings_t         settings;     /* what the machine powers on with */
    card_job_t         job;
    settingsio_state_t cfg;
    uint32_t           cfg_bytes;
    char               cfg_error[48];  /* "" when every line was good   */
    uint32_t           ready_us;     /* core 1's bring-up done, since boot */
} boot_report_t;

extern boot_report_t g_boot;

/* Core 0's per-second window for the perf line (design.md §7.4, §14),
 * in tenths and hundredths so that core 1 formats without floats. */
typedef struct {
    uint32_t seconds;        /* windows closed; 0 until the first        */
    uint32_t busy1000;       /* core 0 outside the pacing wait, of wall  */
    uint32_t guest1000;      /* inside ace_run_field, of wall            */
    uint32_t hpi10;          /* host cycles per guest instruction        */
    uint32_t late;           /* fields started after their deadline      */
} core0_perf_t;

extern volatile core0_perf_t g_c0;

/* The counters a build without the UART can give (design.md §13.5,
 * §15.2 M15): core 0 copies them here once a second, and
 * tools/swd-counters.sh reads the block over SWD with both cores
 * running, since halting core 0 would starve the audio it measures.
 * Every word is cumulative from boot, with one writer. The layout is
 * the script's too: change both, and SWD_LAYOUT with them. */
#define SWD_MAGIC  0x41434531u   /* "ACE1", little-endian in memory */
#define SWD_LAYOUT 1u
typedef struct {
    uint32_t magic, layout;
    uint32_t updates;        /* one a second; the block is current      */
    uint32_t uptime_ms;      /* the board's clock at the update         */
    uint32_t fields;         /* guest fields run: rt is these over time */
    uint32_t late, slips;    /* the timer's pacing; 0 on audio          */
    uint32_t presents, snapshots_dropped;
    uint32_t key_events, keys_lost, polls, i2c_errors;
    uint32_t ed_holes;
    uint32_t underrun_samples, late_refills, consumed;
    uint32_t beeper_overflow, speaker_edges;
    uint32_t busy1000;       /* core 0 outside the pacing wait, last second */
    int32_t  battery;        /* SB_REG_BAT's byte, -1 until read        */
    int32_t  temp_c;         /* the die, INT32_MIN until read           */
    uint32_t screen;         /* the guest's 768 screen bytes, an address */
} swd_counters_t;

extern volatile swd_counters_t g_swd;

/* What the menu changes (design.md §12), written by core 1 while the
 * guest is parked and applied by core 0 when it has the machine back
 * (EL §2.5). Core 1 reads perf_line to draw the perf line. */
typedef struct {
    volatile unsigned volume;      /* 0-8, as settings_t has it       */
    volatile bool     perf_line;
    volatile bool     fast_tape;   /* the trap, or the signal (tapeio.h) */
    volatile bool     reset;       /* the menu's Reset: core 0 clears it */
    volatile bool     power_on;    /* a load failed part-way (§10.5)  */
    /* The game layout over the standard map, NULL for none (§9.4):
     * core 0 takes it after every park (keymapio.h). */
    const keylayout_t *volatile layout;
} ui_t;

extern ui_t g_ui;

/* The board, identified by main() before core 1 starts, for the About
 * page (design.md §12). */
extern board_info_t g_board;

#endif /* PICO_ACE_HANDOFF_H */
