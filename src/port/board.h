/* board.h — clocks and board identification (design.md §3.1).
 *
 * hardware-notes.md §2.1: record physical board identity separately from
 * the SDK build target. A banner saying board=pico2 identifies
 * compilation settings, not the installed module.
 */
#ifndef PICO_ACE_BOARD_H
#define PICO_ACE_BOARD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *sdk_board;      /* what we were compiled for            */
    const char *sdk_platform;
    char        unique_id[17];  /* physical: the module's flash id      */
    uint8_t     chip_version;   /* physical: RP2350 revision, 0 if n/a  */
    uint32_t    clk_sys_hz;
    uint32_t    clk_peri_hz;
    uint32_t    qmi_timing;     /* flash XIP's, as the clock left it     */
    unsigned    vreg_mv;        /* the core rail, as set                 */
} board_info_t;

/* Set clk_sys to 150 MHz with the core rail at its default, and put
 * clk_peri on it (design.md §3.1, §18 item 3). Call it first in main(),
 * before any peripheral is brought up: every one derives its rate from
 * the clock it finds. Returns false if the PLL would not take it. */
bool board_init_clocks(void);

/* The die temperature, hardware-notes.md §8.1: init once, then whole
 * degrees C, uncalibrated, averaged over sixteen conversions. Core 1's. */
void board_temp_init(void);
int  board_temp_c(void);

void board_identify(board_info_t *info);
void board_log_banner(const board_info_t *info);

#endif /* PICO_ACE_BOARD_H */
