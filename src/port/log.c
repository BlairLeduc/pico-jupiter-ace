/* log.c — core 0's UART output, drained by core 1 (design.md §4.3; EL §2.3). */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "config.h"

/* Power of two, so the indices can run free and wrap by mask. */
#define LOG_RING ACE_LOG_RING
_Static_assert((LOG_RING & (LOG_RING - 1u)) == 0, "the log ring must be a power of two");

static char              s_ring[LOG_RING];
static volatile uint32_t s_head;       /* written by core 0 only */
static volatile uint32_t s_tail;       /* written by core 1 only */
static volatile unsigned s_dropped;
static bool              s_mid_line;   /* core 1's: a line half sent */

void log_printf(const char *fmt, ...) {
#if !PICO_ACE_UART
    (void)fmt;          /* nowhere to send it, so not even formatted */
    return;
#else
    char line[ACE_LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((unsigned)n >= sizeof line) n = sizeof line - 1;

    /* The UART wants CR LF, as stdio would have sent (hardware-notes.md
     * §2.7's capture reads either, but a terminal does not). */
    unsigned need = (unsigned)n;
    for (int i = 0; i < n; i++) need += line[i] == '\n';

    uint32_t head = s_head;
    if (LOG_RING - (head - s_tail) < need) {
        s_dropped++;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (line[i] == '\n') s_ring[head++ & (LOG_RING - 1u)] = '\r';
        s_ring[head++ & (LOG_RING - 1u)] = line[i];
    }
    __dmb();                    /* the bytes land before the index moves */
    s_head = head;
#endif
}

void log_pump(void) {
#if !PICO_ACE_UART
    return;             /* the UART was never brought up */
#endif
    uint32_t tail = s_tail;
    uint32_t head = s_head;
    if (tail == head) return;
    __dmb();                    /* read the bytes after seeing the index */
    while (tail != head && uart_is_writable(uart_default)) {
        char c = s_ring[tail++ & (LOG_RING - 1u)];
        uart_putc_raw(uart_default, c);
        s_mid_line = c != '\n';
    }
    __dmb();
    s_tail = tail;
}

void log_core1(const char *fmt, ...) {
#if !PICO_ACE_UART
    (void)fmt;
#else
    /* Core 0 queues whole lines, so the rest of this one is in the ring:
     * at most ACE_LOG_LINE bytes, ~45 ms of UART. */
    while (s_mid_line && s_tail != s_head) log_pump();
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
#endif
}

unsigned log_dropped(void) {
    return s_dropped;
}
