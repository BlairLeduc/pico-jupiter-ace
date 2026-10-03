/* test_bench.c — M2's workloads, run on the host (design.md §15.2 M2).
 *
 *   test_bench              the forth program
 *   test_bench zexdoc.com   the start of ZEXDOC, skipped if not fetched
 *
 * Checks that each program does what it is meant to before the board
 * times it, and prints the T-states and instructions of one timed run
 * (BENCH_RUN_T): the bench firmware runs the same code, so the board must
 * report the same two counts.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "suites.h"
#include "test_util.h"

static bench_t b;

static uint16_t result(void) {
    return (uint16_t)(b.mem[BENCH_FORTH_RESULT] | b.mem[BENCH_FORTH_RESULT + 1] << 8);
}

static void report(const char *name, uint64_t t) {
    printf("%s: %llu T, %lu instructions, %.3f T per instruction, %lu ED holes\n",
           name, (unsigned long long)t, (unsigned long)b.cpu.insns,
           (double)t / b.cpu.insns, (unsigned long)b.cpu.ed_holes);
}

static int forth(void) {
    /* Each pass stores the sum with two writes; watch them through the
     * slow path, and check the stacks are balanced at every store. */
    bench_forth_load(&b);
    b.pages[BENCH_FORTH_RESULT >> 8] = (page_t){ NULL, NULL };
    for (uint32_t pass = 1; pass <= 3; pass++) {
        b.mem[BENCH_FORTH_RESULT] = b.mem[BENCH_FORTH_RESULT + 1] = 0;
        while (b.slow_writes < 2 * pass && b.cpu.t < 100000000u)
            z80_step(&b.cpu);
        CHECK(b.slow_writes == 2 * pass, "pass %u: %u stores", pass, b.slow_writes);
        CHECK(result() == BENCH_FORTH_SUM, "pass %u: sum $%04X", pass, result());
        CHECK(b.cpu.sp == BENCH_FORTH_SP, "pass %u: SP $%04X", pass, b.cpu.sp);
        CHECK(b.cpu.ix.w == BENCH_FORTH_RP, "pass %u: IX $%04X", pass, b.cpu.ix.w);
    }
    uint32_t t_pass = b.cpu.t / 3;
    printf("forth: one pass is %u T, %.1f ms of a 3.25 MHz Ace\n", t_pass,
           t_pass / 3250.0);

    /* The timed run, as the board does it: every page fast. */
    bench_forth_load(&b);
    uint64_t t = bench_run(&b, BENCH_RUN_T);
    CHECK(t >= BENCH_RUN_T && t < BENCH_RUN_T + BENCH_SLICE_T, "ran %llu T",
          (unsigned long long)t);
    CHECK(b.slow_writes == 0, "%u slow writes on a flat bus", b.slow_writes);
    CHECK(result() == BENCH_FORTH_SUM, "sum $%04X", result());
    report("forth", t);
    TEST_DONE();
}

static int zexdoc(const char *name) {
    static uint8_t com[65536];
    FILE *f = suite_open(name, "rb");
    size_t n = fread(com, 1, sizeof com, f);
    fclose(f);
    CHECK(bench_zex_load(&b, com, n), "%s: %zu bytes do not fit", name, n);

    uint64_t t = bench_run(&b, BENCH_RUN_T);
    printf("%s printed: %s\n", name, b.out);
    CHECK(!b.cpu.halted, "reached the warm boot within %u T", BENCH_RUN_T);
    CHECK(strstr(b.out, "instruction exerciser") != NULL, "no banner");
    CHECK(strstr(b.out, "ERROR") == NULL, "a group failed");
    report(name, t);
    TEST_DONE();
}

int main(int argc, char **argv) {
    return argc > 1 ? zexdoc(argv[1]) : forth();
}
