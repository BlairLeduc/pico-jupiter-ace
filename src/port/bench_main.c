/* bench_main.c — M2's firmware: the Z80 alone on the board, timed
 * (design.md §3.2, §15.2 M2).
 *
 * No LCD, keyboard or audio, and one core. Each workload in src/bench/ is
 * loaded afresh and run for BENCH_RUN_T, ten seconds of a 3.25 MHz Ace,
 * with nothing printed while the clock runs. Each run reports host cycles
 * per guest instruction, mean T per instruction, and the share of core 0
 * a 3.25 MHz guest would take at that rate. The T-state and instruction
 * counts must equal test_bench's on the host: the same code ran.
 *
 * Host cycles are microseconds times clk_sys, which is fixed at 150 MHz
 * (design.md §18 item 3) and reported in the banner.
 */
#include <stdbool.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "bench.h"
#include "board.h"
#include "pico_ace_version.h"

#if PICO_ACE_BENCH_ZEXDOC
extern const uint8_t bench_zexdoc[], bench_zexdoc_end[];
#endif

static board_info_t g_board;
static bench_t      g_bench;

static void report(const char *name, unsigned pass, uint64_t t, uint64_t us) {
    const z80_t *c = &g_bench.cpu;
    double cycles = (double)us * clock_get_hz(clk_sys) / 1e6;
    double guest_hz = (double)t / ((double)us / 1e6);
    printf("bench %-6s pass %u: %llu T %lu insn %llu us | %.1f cyc/insn %.3f T/insn "
           "%.2f MHz guest, core 0 %.1f %% at 3.25 MHz\n",
           name, pass, (unsigned long long)t, (unsigned long)c->insns,
           (unsigned long long)us, cycles / c->insns, (double)t / c->insns,
           guest_hz / 1e6, 100.0 * 3.25e6 / guest_hz);
}

static void run(const char *name, unsigned pass) {
    uint64_t t0 = time_us_64();
    uint64_t t = bench_run(&g_bench, BENCH_RUN_T);
    uint64_t us = time_us_64() - t0;
    report(name, pass, t, us);
}

int main(void) {
    bool clocks_ok = board_init_clocks();
    stdio_init_all();
    board_temp_init();
    board_identify(&g_board);

    board_log_banner(&g_board);
    printf("  firmware     : %s (M2 bench)\n", PICO_ACE_VERSION);
    printf("  SRAM tier    : %d\n", PICO_ACE_RAM_TIER);
    if (!clocks_ok)
        printf("  WARNING: clk_sys is not at 150 MHz; the numbers below are not M2's\n");

    /* One core and no deadline: a blocking printf between runs costs
     * nothing that is timed (hardware-notes.md §2.7). */
    for (unsigned pass = 1;; pass++) {
        bench_forth_load(&g_bench);
        run("forth", pass);
        uint16_t sum = (uint16_t)(g_bench.mem[BENCH_FORTH_RESULT] |
                                  g_bench.mem[BENCH_FORTH_RESULT + 1] << 8);
        if (sum != BENCH_FORTH_SUM)
            printf("bench forth  pass %u: WRONG sum $%04X, not $%04X\n", pass, sum,
                   BENCH_FORTH_SUM);

#if PICO_ACE_BENCH_ZEXDOC
        if (bench_zex_load(&g_bench, bench_zexdoc,
                           (size_t)(bench_zexdoc_end - bench_zexdoc))) {
            run("zexdoc", pass);
            if (pass == 1)
                printf("bench zexdoc printed: %s\n", g_bench.out);
        }
#else
        if (pass == 1)
            printf("bench zexdoc: not in this image; run tools/fetch-test-suites.sh "
                   "and rebuild\n");
#endif
        printf("bench die %d C\n", board_temp_c());
    }
}
