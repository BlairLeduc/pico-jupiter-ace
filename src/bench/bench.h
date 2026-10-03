/* bench.h — M2's workloads: the Z80 alone on a flat 64 KiB bus
 * (design.md §3.2, §15.2 M2).
 *
 * Portable, like src/core/: no SDK header and no allocation. The host test
 * (test/host/test_bench.c) and the bench firmware (src/port/bench_main.c)
 * load and run the same programs, so the T-states and instruction counts
 * the board reports for a budget are the ones the host checked.
 *
 *   forth    an indirect-threaded inner interpreter, NEXT and a few
 *            primitives in a DO ... LOOP, standing in for the Ace ROM's
 *            Forth until M3 runs the real one
 *   zexdoc   the start of ZEXDOC under the same BDOS stub as the host's
 *            test_z80_zex: a CRC-heavy instruction mix, a control that is
 *            not Forth-shaped
 */
#ifndef PICO_ACE_BENCH_H
#define PICO_ACE_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "z80.h"

/* One Ace field, near enough (design.md §11.1): the slice each z80_run
 * call is given, so the loop has the shape the machine's will. */
#define BENCH_SLICE_T        65000u

/* One timed run: 500 fields, ten seconds of a 3.25 MHz Ace. */
#define BENCH_RUN_T          (500u * BENCH_SLICE_T)

/* The forth program's layout. Each pass of its outer loop stores the sum
 * of 2i for i in 0..999, modulo 2^16, at BENCH_FORTH_RESULT, which is on
 * a page of its own so a test can watch it through the slow path. */
#define BENCH_FORTH_RESULT   0xC000u
#define BENCH_FORTH_SUM      0x3E58u    /* 999,000 mod 65,536 */
#define BENCH_FORTH_SP       0xF000u    /* data stack: the machine stack  */
#define BENCH_FORTH_RP       0xE000u    /* return stack: IX               */

typedef struct {
    uint8_t  mem[65536];
    page_t   pages[Z80_PAGE_COUNT];
    z80_t    cpu;

    char     out[256];           /* what the BDOS printed, truncated      */
    size_t   out_len;
    uint32_t slow_writes;        /* writes that took the bus's slow path  */
} bench_t;

/* Clear the machine, load the forth program, and point the CPU at it.
 * Every page takes the fast path. */
void bench_forth_load(bench_t *b);

/* Clear the machine and load a CP/M .com (ZEXDOC) at $0100 under a BDOS
 * stub that prints functions 2 and 9 into b->out. False if it does not
 * fit. Every page takes the fast path. */
bool bench_zex_load(bench_t *b, const uint8_t *com, size_t len);

/* Run in BENCH_SLICE_T slices until at least t_states have passed, or the
 * CPU halts (the .com's warm boot), and return the T-states run. */
uint64_t bench_run(bench_t *b, uint64_t t_states);

#endif /* PICO_ACE_BENCH_H */
