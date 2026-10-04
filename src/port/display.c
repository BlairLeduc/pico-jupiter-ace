/* display.c — what core 1 puts on the panel (design.md §7). */

#include "display.h"

#include "config.h"
#include "lcd.h"

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3))

_Static_assert(ACE_SCREEN_X + ACE_SCREEN_W <= ACE_PANEL_W &&
               ACE_SCREEN_Y + ACE_SCREEN_H <= ACE_PANEL_H,
               "the guest must fit on the panel");

void display_test_pattern(void) {
    const unsigned x = ACE_SCREEN_X, y = ACE_SCREEN_Y;
    const unsigned w = ACE_SCREEN_W, h = ACE_SCREEN_H;
    const unsigned pw = ACE_PANEL_W, ph = ACE_PANEL_H;
    const uint16_t white = RGB565(0xFF, 0xFF, 0xFF);
    const uint16_t grey = RGB565(0x80, 0x80, 0x80);

    lcd_fill(0, 0, pw, ph, 0x0000);

    lcd_fill(0, 0, pw, 1, grey);
    lcd_fill(0, ph - 1u, pw, 1, grey);
    lcd_fill(0, 0, 1, ph, grey);
    lcd_fill(pw - 1u, 0, 1, ph, grey);

    lcd_fill(x, y, w, 1, white);               /* top    */
    lcd_fill(x, y + h - 1u, w, 1, white);      /* bottom */
    lcd_fill(x, y, 1, h, white);               /* left   */
    lcd_fill(x + w - 1u, y, 1, h, white);      /* right  */

    lcd_fill(x + 2u, y + 2u, 16, 16, RGB565(0xFF, 0x00, 0x00));
    lcd_fill(x + w - 18u, y + 2u, 16, 16, RGB565(0x00, 0xFF, 0x00));
    lcd_fill(x + 2u, y + h - 18u, 16, 16, RGB565(0x00, 0x00, 0xFF));
    lcd_fill(x + w - 18u, y + h - 18u, 16, 16, RGB565(0xFF, 0xFF, 0x00));
}
