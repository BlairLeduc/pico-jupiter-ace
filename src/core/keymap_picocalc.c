/* keymap_picocalc.c — PicoCalc key codes to Ace matrix cells (design.md §9.2).
 *
 * The cells are the ROM's, not a transcription (§9.3). Each of the 40
 * cells was pressed at the prompt alone, with SHIFT and with SYMBOL
 * SHIFT, and what the ROM typed was read back (2026-10-03); test_keyboard
 * repeats that sweep and types every entry below through the ROM.
 *
 *   half-row  D0      D1      D2   D3   D4      SYMBOL SHIFT gives
 *   $FE  0    SHIFT   SYMBOL  Z    X    C       :  £  ?
 *   $FD  1    A       S       D    F    G       ~  |  \  {  }
 *   $FB  2    Q       W       E    R    T       (Q W E capitals)  <  >
 *   $F7  3    1       2       3    4    5       !  @  #  $  %
 *   $EF  4    0       9       8    7    6       _  )  (  '  &
 *   $DF  5    P       O       I    U    Y       "  ;  ©  ]  [
 *   $BF  6    ENTER   L       K    J    H       (ENTER)  =  +  -  ^
 *   $7F  7    SPACE   M       N    B    V       (SPACE)  .  ,  *  /
 *
 * Letters are lower case unshifted at power-on. SHIFT with a digit is
 * the editing set: 1 DELETE LINE, 2 CAPS LOCK, 4 INVERSE VIDEO (a
 * toggle), 5 left, 6 up, 7 down, 8 right, 9 GRAPHICS, 0 DELETE. SHIFT+3
 * types 3: this ROM has no TRUE VIDEO key. SHIFT+SPACE is BREAK, which a
 * running word tests for; at the prompt it types a space.
 *
 * Code values are the southbridge firmware's (hardware-notes.md §6;
 * keyboard.h in the PicoCalc repository).
 */

#include "keymatrix.h"

/* Ace keys as "row, col". */
#define AK_Z     0, 2
#define AK_X     0, 3
#define AK_C     0, 4
#define AK_A     1, 0
#define AK_S     1, 1
#define AK_D     1, 2
#define AK_F     1, 3
#define AK_G     1, 4
#define AK_Q     2, 0
#define AK_W     2, 1
#define AK_E     2, 2
#define AK_R     2, 3
#define AK_T     2, 4
#define AK_1     3, 0
#define AK_2     3, 1
#define AK_3     3, 2
#define AK_4     3, 3
#define AK_5     3, 4
#define AK_0     4, 0
#define AK_9     4, 1
#define AK_8     4, 2
#define AK_7     4, 3
#define AK_6     4, 4
#define AK_P     5, 0
#define AK_O     5, 1
#define AK_I     5, 2
#define AK_U     5, 3
#define AK_Y     5, 4
#define AK_ENTER 6, 0
#define AK_L     6, 1
#define AK_K     6, 2
#define AK_J     6, 3
#define AK_H     6, 4
#define AK_SPACE 7, 0
#define AK_M     7, 1
#define AK_N     7, 2
#define AK_B     7, 3
#define AK_V     7, 4

/* The editing keys, SHIFT with a digit. */
#define AK_DELETE_LINE AK_1
#define AK_CAPS_LOCK   AK_2
#define AK_INVERSE     AK_4
#define AK_CUR_LEFT    AK_5
#define AK_CUR_UP      AK_6
#define AK_CUR_DOWN    AK_7
#define AK_CUR_RIGHT   AK_8
#define AK_GRAPHICS    AK_9
#define AK_DELETE      AK_0
#define AK_BREAK       AK_SPACE

#define NOCELL 0, 0

#define LETTER(lc, uc, cell) \
    { lc, cell, 0 }, { uc, cell, KM_SHIFT }

const keymap_t keymap_picocalc[] = {
    /* The MCU sends lower case unshifted and upper case shifted, and so
     * does the Ace: PicoCalc Shift is Ace SHIFT, and the PicoCalc's own
     * Caps Lock works through it. */
    LETTER('a', 'A', AK_A), LETTER('b', 'B', AK_B), LETTER('c', 'C', AK_C),
    LETTER('d', 'D', AK_D), LETTER('e', 'E', AK_E), LETTER('f', 'F', AK_F),
    LETTER('g', 'G', AK_G), LETTER('h', 'H', AK_H), LETTER('i', 'I', AK_I),
    LETTER('j', 'J', AK_J), LETTER('k', 'K', AK_K), LETTER('l', 'L', AK_L),
    LETTER('m', 'M', AK_M), LETTER('n', 'N', AK_N), LETTER('o', 'O', AK_O),
    LETTER('p', 'P', AK_P), LETTER('q', 'Q', AK_Q), LETTER('r', 'R', AK_R),
    LETTER('s', 'S', AK_S), LETTER('t', 'T', AK_T), LETTER('u', 'U', AK_U),
    LETTER('v', 'V', AK_V), LETTER('w', 'W', AK_W), LETTER('x', 'X', AK_X),
    LETTER('y', 'Y', AK_Y), LETTER('z', 'Z', AK_Z),

    { '0', AK_0, 0 }, { '1', AK_1, 0 }, { '2', AK_2, 0 }, { '3', AK_3, 0 },
    { '4', AK_4, 0 }, { '5', AK_5, 0 }, { '6', AK_6, 0 }, { '7', AK_7, 0 },
    { '8', AK_8, 0 }, { '9', AK_9, 0 },
    { ' ', AK_SPACE, 0 },

    /* Punctuation by what it is, wherever each keyboard puts it: the Ace
     * shifts all of it with SYMBOL SHIFT, the PicoCalc some of it with
     * Shift, which the held set keeps off the matrix (§9.2). */
    { '!',  AK_1, KM_SYM }, { '@', AK_2, KM_SYM }, { '#', AK_3, KM_SYM },
    { '$',  AK_4, KM_SYM }, { '%', AK_5, KM_SYM }, { '&', AK_6, KM_SYM },
    { '\'', AK_7, KM_SYM }, { '(', AK_8, KM_SYM }, { ')', AK_9, KM_SYM },
    { '_',  AK_0, KM_SYM },
    { ':',  AK_Z, KM_SYM }, { '?', AK_C, KM_SYM },
    { '~',  AK_A, KM_SYM }, { '|', AK_S, KM_SYM }, { '\\', AK_D, KM_SYM },
    { '{',  AK_F, KM_SYM }, { '}', AK_G, KM_SYM },
    { '<',  AK_R, KM_SYM }, { '>', AK_T, KM_SYM },
    { '"',  AK_P, KM_SYM }, { ';', AK_O, KM_SYM },
    { ']',  AK_U, KM_SYM }, { '[', AK_Y, KM_SYM },
    { '=',  AK_L, KM_SYM }, { '+', AK_K, KM_SYM }, { '-', AK_J, KM_SYM },
    { '^',  AK_H, KM_SYM },
    { '.',  AK_M, KM_SYM }, { ',', AK_N, KM_SYM }, { '*', AK_B, KM_SYM },
    { '/',  AK_V, KM_SYM },
    /* The Ace has '£' where ASCII has '`' ($60, §7.5), and the PicoCalc
     * has no '£': the code is the same, so the key is too. '©' ($7F) has
     * no PicoCalc key; Ctrl+i reaches it. */
    { '`',  AK_X, KM_SYM },

    { PICOCALC_KEY_ENTER,     AK_ENTER, 0 },
    { PICOCALC_KEY_BACKSPACE, AK_DELETE, KM_SHIFT },
    { PICOCALC_KEY_DEL,       AK_DELETE, KM_SHIFT },

    /* Guest SHIFT is ours to assert, so the host's swallowed Shift+arrow
     * chords cost nothing (hardware-notes.md §6.3). The ROM moves up on
     * SHIFT+6 and down on SHIFT+7. */
    { PICOCALC_KEY_LEFT,  AK_CUR_LEFT,  KM_SHIFT },
    { PICOCALC_KEY_UP,    AK_CUR_UP,    KM_SHIFT },
    { PICOCALC_KEY_DOWN,  AK_CUR_DOWN,  KM_SHIFT },
    { PICOCALC_KEY_RIGHT, AK_CUR_RIGHT, KM_SHIFT },

    /* BREAK needs a plain key: the host's Shift+Space never arrives
     * (hardware-notes.md §6.3). Shift+Esc arrives as Break, and is BREAK
     * too. */
    { PICOCALC_KEY_ESC,   AK_BREAK, KM_SHIFT },
    { PICOCALC_KEY_BREAK, AK_BREAK, KM_SHIFT },

    /* The Alt layer (§9.2). The MCU skips its lower-casing when Alt is
     * down, so these arrive as capitals. Alt+, . Space and B never
     * arrive, and Alt+I is the MCU's Insert (hardware-notes.md §6.3). */
    { 'L', AK_CAPS_LOCK,   KM_ALT | KM_SHIFT },
    { 'G', AK_GRAPHICS,    KM_ALT | KM_SHIFT },
    { 'V', AK_INVERSE,     KM_ALT | KM_SHIFT },
    { 'X', AK_DELETE_LINE, KM_ALT | KM_SHIFT },
    { 'M', KM_PAGE_MAIN, 0, KM_ALT | KM_MENU },
    { 'P', NOCELL,          KM_ALT | KM_PAUSE },
    { 'R', NOCELL,          KM_ALT | KM_RESET },

    /* F1-F5 and F10 open the menu at a page (§12). The Ace has no
     * function keys, so they are the menu's everywhere. */
    { PICOCALC_KEY_F1 + 0, KM_PAGE_TAPE,     0, KM_MENU },
    { PICOCALC_KEY_F1 + 1, KM_PAGE_SNAPSHOT, 0, KM_MENU },
    { PICOCALC_KEY_F1 + 2, KM_PAGE_MACHINE,  0, KM_MENU },
    { PICOCALC_KEY_F1 + 3, KM_PAGE_LAYOUT,   0, KM_MENU },
    { PICOCALC_KEY_F1 + 4, KM_PAGE_ABOUT,    0, KM_MENU },
    { PICOCALC_KEY_F10,    KM_PAGE_MAIN,     0, KM_MENU },
};

const size_t keymap_picocalc_len = sizeof keymap_picocalc / sizeof keymap_picocalc[0];

/* PicoCalc keys that exist only as another's shifted alternate
 * (hardware-notes.md §6.3), by the key they are on. */
#define PC_INSERT    0xD1u
#define PC_HOME      0xD2u
#define PC_END       0xD5u
#define PC_PAGE_UP   0xD6u
#define PC_PAGE_DOWN 0xD7u
#define PC_TAB       0x09u

uint8_t keymap_picocalc_canonical(uint8_t code) {
    if (code >= 'A' && code <= 'Z') return (uint8_t)(code + ('a' - 'A'));

    /* Each key's shifted alternate back to its base (keyboard.ino). */
    switch (code) {
    case '!': return '1';  case '@': return '2';  case '#': return '3';
    case '$': return '4';  case '%': return '5';  case '^': return '6';
    case '&': return '7';  case '*': return '8';  case '(': return '9';
    case ')': return '0';  case '_': return '-';  case '+': return '=';
    case '|': return '\\'; case '?': return '/';  case ':': return ';';
    case '"': return '\''; case '<': return ',';  case '>': return '.';
    case '{': return '[';  case '}': return ']';  case '~': return '`';
    case PC_END:             return PICOCALC_KEY_DEL;
    case PC_HOME:            return PC_TAB;
    case PICOCALC_KEY_BREAK: return PICOCALC_KEY_ESC;
    case PC_INSERT:          return PICOCALC_KEY_ENTER;
    case PC_PAGE_UP:         return PICOCALC_KEY_UP;
    case PC_PAGE_DOWN:       return PICOCALC_KEY_DOWN;
    default:                 return code;
    }
}
