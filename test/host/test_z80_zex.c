/* test_z80_zex.c — Frank Cringle's ZEXDOC and ZEXALL (design.md §5.4).
 *
 *   test_z80_zex zexdoc.com     documented flags only
 *   test_z80_zex zexall.com     every flag, X and Y included
 *
 * The program runs on a flat 64 KiB bus under a minimal CP/M: BDOS at
 * $0005 jumps to a stub that traps to the host through an OUT, which
 * prints for functions 2 and 9, and the warm boot at $0000 is a HALT. Each
 * group prints "OK" or "ERROR"; the run passes only when it reaches
 * "Tests complete" with no ERROR. It takes minutes, which is the M1
 * measurement: the wall time, a regression marker only (§15.2).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "suites.h"
#include "test_util.h"
#include "z80.h"

#define BDOS_STUB 0xFE00u   /* also the top of the TPA: ZEX puts SP here */
#define TRAP_PORT 0x00u

static uint8_t mem[65536];
static page_t  pages[Z80_PAGE_COUNT];
static z80_t   cpu;

static char    output[16384];
static size_t  out_len;

static void put(char ch) {
    putchar(ch);
    if (ch == '\n')
        fflush(stdout);
    if (out_len + 1 < sizeof output)
        output[out_len++] = ch;
}

/* The BDOS: C is the function, E the character, DE the string. */
static void bdos(z80_t *c) {
    switch (c->bc.b.l) {
    case 2:
        put((char)c->de.b.l);
        break;
    case 9:
        for (uint16_t a = c->de.w; mem[a] != '$'; a++)
            put((char)mem[a]);
        break;
    default:
        break;
    }
}

static uint8_t mem_read(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void mem_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }
static uint8_t io_read(void *ctx, uint16_t port) { (void)ctx; (void)port; return 0xFF; }

static void io_write(void *ctx, uint16_t port, uint8_t v) {
    (void)v;
    if ((port & 0xFFu) == TRAP_PORT)
        bdos((z80_t *)ctx);
}

static double now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "zexdoc.com";
    FILE *f = suite_open(name, "rb");
    size_t n = fread(mem + 0x100, 1, sizeof mem - 0x100 - 0x200, f);
    fclose(f);
    CHECK(n > 0, "%s is empty", name);

    mem[0x0000] = 0x76;                              /* warm boot: HALT       */
    mem[0x0005] = 0xC3;                              /* JP BDOS_STUB          */
    mem[0x0006] = BDOS_STUB & 0xFF;
    mem[0x0007] = BDOS_STUB >> 8;
    mem[BDOS_STUB + 0] = 0xD3;                       /* OUT (TRAP_PORT),A     */
    mem[BDOS_STUB + 1] = TRAP_PORT;
    mem[BDOS_STUB + 2] = 0xC9;                       /* RET                   */

    /* The fast path for every page: the slow path is never taken. */
    for (unsigned p = 0; p < Z80_PAGE_COUNT; p++)
        pages[p] = (page_t){ mem + p * 256u, mem + p * 256u };

    cpu.bus = (z80_bus_t){ pages, &cpu, mem_read, mem_write, io_read, io_write, NULL, NULL };
    z80_reset(&cpu);
    cpu.pc = 0x0100;
    cpu.sp = BDOS_STUB;

    double t0 = now();
    uint64_t t_states = 0;
    while (!cpu.halted)
        t_states += z80_run(&cpu, 1u << 24);
    double secs = now() - t0;

    printf("\n%s: %.1f s, %llu T (%.0f guest MHz)\n", name, secs,
           (unsigned long long)t_states, t_states / secs / 1e6);
    output[out_len] = 0;
    CHECK(cpu.pc == 0x0000, "halted at %04x, not at the warm boot", cpu.pc);
    CHECK(strstr(output, "Tests complete") != NULL, "did not reach the end");
    CHECK(strstr(output, "ERROR") == NULL, "a group failed");
    TEST_DONE();
}
