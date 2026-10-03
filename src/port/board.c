/* board.c — clocks and board identification (design.md §3.1). */

#include "board.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#if PICO_RP2350
#include "hardware/structs/qmi.h"
#include "hardware/structs/sysinfo.h"
#endif

/* The stock 150 MHz, the RP2350's rated clock and one of the two points
 * where the SPI divider delivers the full 75 MHz to the panel
 * (hardware-notes.md §3). The 300 MHz option is deferred (design.md §18
 * item 3); pico-atom's board.c has its sequence if it comes back. */
#define BOARD_MHZ 150u

bool board_init_clocks(void) {
    if (clock_get_hz(clk_sys) != BOARD_MHZ * 1000000u &&
        !set_sys_clock_khz(BOARD_MHZ * 1000u, false)) {
        return false;
    }
#if PICO_RP2350
    /* The rail is not reset with the chip: after another firmware ran
     * overclocked, a watchdog or SWD reset comes back to 150 still at its
     * raised voltage. Put it back once the clock is down, never before
     * (hardware-notes.md §3). The flash's timing needs nothing: the
     * bootrom sets it again. */
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
#endif

    /* set_sys_clock_pll otherwise parks clk_peri on the 48 MHz USB PLL,
     * and clk_peri does not follow clk_sys by default. Every derived
     * clock — the SPI baud rate and the audio PWM carrier — is re-applied
     * by its own driver after this returns (hardware-notes.md §3). */
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys),
                    clock_get_hz(clk_sys));
    return true;
}

/* The die's sensor (hardware-notes.md §8.1). Its ADC input is the last:
 * 4 on the QFN-60 RP2350A, 8 on the QFN-80 RP2350B. The SDK takes that
 * from the board header, and this build says pico2 on a Plus 2 W, whose
 * RP2350B would put input 4 on a floating GPIO that reads as a plausible
 * number. So the package decides, read off the chip. */
static unsigned s_temp_input;

void board_temp_init(void) {
    adc_init();   /* touches no GPIO: the audio PWM on GP26/27 is safe */
    adc_set_temp_sensor_enabled(true);
#if PICO_RP2350
    s_temp_input = (sysinfo_hw->package_sel & SYSINFO_PACKAGE_SEL_BITS) ? 4u : 8u;
#else
    s_temp_input = 4u;
#endif
}

int board_temp_c(void) {
    /* Written directly, not with adc_select_input, whose parameter check
     * is the board header's channel count. */
    hw_write_masked(&adc_hw->cs, s_temp_input << ADC_CS_AINSEL_LSB, ADC_CS_AINSEL_BITS);
    uint32_t sum = 0;
    for (unsigned i = 0; i < 16u; i++) sum += adc_read();   /* ~32 us */
    /* The datasheet's formula, uncalibrated, over the 3.3 V reference:
     * whole degrees are as much as it can say (§8.1). */
    float v = (float)sum * (3.3f / 4096.0f / 16.0f);
    float t = 27.0f - (v - 0.706f) / 0.001721f;
    return (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

void board_identify(board_info_t *info) {
    memset(info, 0, sizeof(*info));

    info->sdk_board = PICO_BOARD;
#if PICO_RP2350
    info->sdk_platform = "rp2350";
    info->chip_version = (uint8_t)((sysinfo_hw->chip_id >> 28) & 0xFu);
#elif PICO_RP2040
    info->sdk_platform = "rp2040";
    info->chip_version = 0;
#else
    info->sdk_platform = "unknown";
    info->chip_version = 0;
#endif

    pico_get_unique_board_id_string(info->unique_id, sizeof(info->unique_id));

    info->clk_sys_hz  = clock_get_hz(clk_sys);
    info->clk_peri_hz = clock_get_hz(clk_peri);
#if PICO_RP2350
    info->qmi_timing  = qmi_hw->m[0].timing;
    info->vreg_mv     = 550u + 50u * (unsigned)vreg_get_voltage();
#endif
}

void board_log_banner(const board_info_t *info) {
    printf("\npico-ace — Jupiter Ace for the PicoCalc\n");
    /* Build target and physical identity, reported separately and
     * labelled as such (hardware-notes.md §2.1). */
    printf("  build target : board=%s platform=%s\n",
           info->sdk_board, info->sdk_platform);
    printf("  module       : id=%s chip_rev=%u\n",
           info->unique_id, info->chip_version);
#if PICO_RP2350
    /* Which ADC input the die's sensor is (board_temp_init). */
    printf("  package      : %s, temperature on ADC input %u\n",
           (sysinfo_hw->package_sel & SYSINFO_PACKAGE_SEL_BITS) ? "QFN-60 (RP2350A)"
                                                                 : "QFN-80 (RP2350B)",
           (sysinfo_hw->package_sel & SYSINFO_PACKAGE_SEL_BITS) ? 4u : 8u);
#endif
    printf("  clocks       : clk_sys=%lu Hz clk_peri=%lu Hz\n",
           (unsigned long)info->clk_sys_hz, (unsigned long)info->clk_peri_hz);
#if PICO_RP2350
    unsigned div = (info->qmi_timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    printf("  flash, core  : QMI clkdiv %u rxdelay %lu (%lu kHz), core rail ~%u mV\n", div,
           (unsigned long)((info->qmi_timing & QMI_M0_TIMING_RXDELAY_BITS) >>
                           QMI_M0_TIMING_RXDELAY_LSB),
           (unsigned long)(div ? info->clk_sys_hz / div / 1000u : 0u), info->vreg_mv);
#endif
}
