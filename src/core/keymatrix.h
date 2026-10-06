/* keymatrix.h — host key events to the Ace's key matrix (design.md §9).
 *
 * The PicoCalc delivers translated characters as press/release events
 * (hardware-notes.md §6.2); the Ace wants an 8x5 matrix whose first
 * half-row holds SHIFT and SYMBOL SHIFT. This builds a held-key set from
 * the events and drives the matrix from that set once per field (§9.1),
 * never from a character stream.
 *
 * Runs on core 0, which owns ace_t. Events reach it through the port's
 * queue from core 1, which owns the I2C bus (§4.3).
 */
#ifndef PICO_ACE_KEYMATRIX_H
#define PICO_ACE_KEYMATRIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ace.h"
#include "config.h"

/* ---- the keymap: data (§9.2) ----------------------------------------- */

#define KM_SHIFT  0x01u   /* assert the Ace's SHIFT with this cell       */
#define KM_ALT    0x02u   /* the entry is on the Alt layer               */
#define KM_SYM    0x04u   /* assert SYMBOL SHIFT with this cell          */
#define KM_RESET  0x08u   /* no cell: ask to reset the machine (§12)     */
#define KM_MENU   0x10u   /* no cell: the emulator menu (§12)            */
#define KM_PAUSE  0x80u   /* no cell: pause the guest (§12)              */
#define KM_NOCELL (KM_RESET | KM_MENU | KM_PAUSE)

/* The menu's pages (§12), as pico-atom has them: KM_MENU's row is the
 * one it opens. F2 is pico-atom's Discs, and the Ace has no disc. */
#define KM_PAGE_MAIN     0u
#define KM_PAGE_TAPE     1u
#define KM_PAGE_SNAPSHOT 2u
#define KM_PAGE_SETUP    3u
#define KM_PAGE_MACHINE  4u
#define KM_PAGE_HELP     5u
#define KM_PAGE_ABOUT    6u

/* SHIFT and SYMBOL SHIFT are the first half-row's D0 and D1 (§2.4). A
 * table entry never names them as its cell: they are its flags. */
#define AK_ROW_MODS  0u
#define AK_COL_SHIFT 0u
#define AK_COL_SYM   1u

typedef struct {
    uint8_t code;         /* host key code, as translated by the MCU     */
    uint8_t row, col;     /* Ace half-row (A8-A15) and bit (D0-D4);
                           * under KM_MENU the row is the page          */
    uint8_t flags;
} keymap_t;

extern const keymap_t keymap_picocalc[];
extern const size_t   keymap_picocalc_len;

/* The physical key behind a translated code. The MCU retranslates at
 * every transition, so releasing Shift first turns press 'A' into
 * release 'a' and press '!' into release '1' (hardware-notes.md §6.2);
 * held-state identity is this, not the code. */
uint8_t keymap_picocalc_canonical(uint8_t code);

/* ---- game layouts: overlays on the standard map (§9.4) --------------- */

/* A binding's code is canonical (keymap_picocalc_canonical), so it holds
 * whichever way the MCU translated the key; its target is one Ace key,
 * flags 0, SHIFT and SYMBOL SHIFT included as the cells they are. */
typedef struct {
    char     name[ACE_KEYMAP_NAME_LEN + 1];
    uint8_t  n;
    keymap_t bind[ACE_KEYMAP_BINDINGS];
    /* Files whose loading selects this layout: a .tap put in the deck or
     * an .ace loaded, named without the extension (§9.4). */
    uint8_t  n_tapes;
    char     tapes[ACE_KEYMAP_TAPES][ACE_KEYMAP_TAPE_LEN + 1];
} keylayout_t;

extern const keylayout_t keylayout_builtin[];
extern const size_t      keylayout_builtin_len;

typedef enum {
    KL_OK = 0,
    KL_SYNTAX,          /* not "word = value", or an empty value       */
    KL_BAD_KEY,         /* no PicoCalc key by that name                */
    KL_BAD_TARGET,      /* no Ace key by that name                     */
    KL_DUPLICATE,       /* a key bound twice                           */
    KL_TOO_MANY,        /* more bindings or tapes than config.h allows */
    KL_TOO_LONG,        /* a name longer than the menu shows           */
} keylayout_status_t;

/* A .map file's text (§9.4): "name = ...", "tapes = ...", and one
 * "<PicoCalc key> = <Ace key>" per line; '#' starts a comment line.
 * `name` is used when the file gives none. On failure *line is the
 * 1-based line at fault, and `out` must not be used. */
keylayout_status_t keylayout_parse(keylayout_t *out, const char *name,
                                   const char *text, size_t len, unsigned *line);
const char *keylayout_status_str(keylayout_status_t st);

/* Does loading this file select the layout? The path's last part, less
 * its extension, against the tapes line, ignoring case: the file is
 * typed by hand. */
bool keylayout_for_file(const keylayout_t *l, const char *path);

/* The names the parser takes, which are the keymap's to know. Both
 * compare without regard to case. */
bool keymap_picocalc_key_named(const char *name, uint8_t *code);
bool keymap_ace_target_named(const char *name, keymap_t *out);

/* A binding as a .map line has it, "left=5", for the menu: the names
 * above, so that the text parses back to the binding. */
void keymap_binding_str(const keymap_t *e, char *out, size_t n);

/* Modifier codes and event states (hardware-notes.md §6.2). */
#define PICOCALC_KEY_ALT      0xA1u
#define PICOCALC_KEY_SHIFT_L  0xA2u
#define PICOCALC_KEY_SHIFT_R  0xA3u
#define PICOCALC_KEY_CTRL     0xA5u

/* F1-F5 unshifted; Shift makes them F6-F10, 0x86-0x90 (keyboard.h). */
#define PICOCALC_KEY_F1       0x81u
#define PICOCALC_KEY_F10      0x90u

/* The other keys this table binds (keyboard.h). */
#define PICOCALC_KEY_BACKSPACE 0x08u
#define PICOCALC_KEY_ENTER     0x0Au
#define PICOCALC_KEY_ESC       0xB1u
#define PICOCALC_KEY_LEFT      0xB4u
#define PICOCALC_KEY_UP        0xB5u
#define PICOCALC_KEY_DOWN      0xB6u
#define PICOCALC_KEY_RIGHT     0xB7u
#define PICOCALC_KEY_BREAK     0xD0u   /* Shift+Esc */
#define PICOCALC_KEY_INSERT    0xD1u   /* Shift+Enter, or Alt+I */
#define PICOCALC_KEY_DEL       0xD4u

#define KEY_EV_PRESSED   1u
#define KEY_EV_HELD      2u
#define KEY_EV_RELEASED  3u

/* One southbridge FIFO entry (hardware-notes.md §6.2). */
typedef struct { uint8_t state, code; } picocalc_event_t;

/* The events a PicoCalc sends for one ASCII byte, so that text from the
 * UART or a host test arrives the way typing would (design.md §9.1).
 * Printable characters are themselves, inside Shift where the PicoCalc
 * types them as a Shift chord; CR and LF are Enter, BS and DEL Backspace,
 * ESC is Esc, and the other control characters are Ctrl with a letter.
 * Returns the count, 0 for a byte no key sends. */
unsigned keymap_picocalc_text(uint8_t ch, picocalc_event_t out[ACE_KEY_TEXT_EVENTS]);

/* ---- the held-key set ------------------------------------------------ */

typedef struct {
    uint8_t  canon;       /* keymap_picocalc_canonical() of the press */
    uint8_t  fields;      /* fields it has been down for */
    /* The binding chosen at press, copied: its release undoes exactly
     * that, whatever is in force by then (EL §7.1). */
    keymap_t map;
    /* Pressed under the host's Shift for a character the Ace types
     * without SHIFT, like '!' (SYMBOL SHIFT + 1): while it is down the
     * host's Shift does not reach the matrix (§9.2). */
    bool unshift;
} keymatrix_held_t;

typedef struct {
    uint8_t state, code;
    uint8_t canon;        /* the physical key, decided as the event arrived */
} keymatrix_event_t;

typedef struct {
    /* Events wait here and are replayed at field rate, in order. */
    keymatrix_event_t queue[ACE_KEY_EVENT_QUEUE];
    uint8_t q_head, q_len;
    uint8_t gap;          /* fields before the next press may apply */
    uint32_t dropped;     /* presses refused for want of room */

    /* Keys whose press is queued or applied and whose release has not
     * arrived yet. The queue always keeps a slot for each of their
     * releases: a lost press drops a character, a lost release holds a
     * key down for ever. */
    uint8_t open[ACE_KEY_EVENT_QUEUE];
    uint8_t n_open;

    keymatrix_held_t held[ACE_KEY_HELD_MAX];
    uint8_t n;
    bool alt, ctrl;
    uint8_t shift;        /* the host's Shifts that are down: bit 0 left, 1 right */
    /* Fields each of left Shift, right Shift and Ctrl has been down: they
     * reach the matrix, so a tap is held as long as a key (§9.1). */
    uint8_t mod_fields[3];

    /* Alt as the events arrive, ahead of the replay: it decides which
     * key an Insert is (keymatrix_event). */
    bool ev_alt;

    /* The game layout over the standard map, or NULL (§9.4). */
    const keylayout_t *layout;

    /* Set on a press, cleared by whoever acts on it. */
    bool menu_request;
    uint8_t menu_page;    /* with menu_request: the entry's row */
    bool pause_request;
    bool reset_request;   /* the port asks before it resets (§9.2) */
} keymatrix_t;

/* Empty, with no layout. */
void keymatrix_init(keymatrix_t *k);

/* Takes effect from the next press; a key already down keeps the binding
 * it went down with. NULL is the standard map. */
void keymatrix_set_layout(keymatrix_t *k, const keylayout_t *l);

/* One [state, code] event off the southbridge FIFO. Queued, not applied:
 * see keymatrix_field. A press that finds no room for itself and for
 * every outstanding release is refused, and its release with it; a
 * press of a key already down (the MCU's auto-repeat) is absorbed.
 *
 * The key an event belongs to is keymap_picocalc_canonical's, except
 * Insert, which the MCU sends for Shift+Enter and for Alt+I
 * (hardware-notes.md §6.3): with Alt down it is the I key, so a release
 * that arrives as 'i' after Alt has gone up still finds its press. */
void keymatrix_event(keymatrix_t *k, uint8_t state, uint8_t code);

/* Once per field, before the guest runs: replay queued events, drive the
 * matrix from the held set, and age it. Shift and Ctrl are paced like
 * keys: their release waits ACE_KEY_MIN_FIELDS, so a tap within one poll
 * still reaches the guest, which may read SHIFT alone.
 *
 * Replay is paced for the ROM, not for the poll. A press and its release
 * can arrive in the same 30 Hz poll (EL §7.1), and the ROM's scan takes a
 * key only on its third field and only after a field with every key up
 * (ROM $0310), so a release waits until its key has been down
 * ACE_KEY_MIN_FIELDS, and a press waits ACE_KEY_GAP_FIELDS after a
 * release. Keys that overlapped at the keyboard still overlap here;
 * keys that did not, do not. */
void keymatrix_field(keymatrix_t *k, ace_t *m);

/* Nothing queued and nothing held: whatever was typed has reached the
 * guest. */
static inline bool keymatrix_idle(const keymatrix_t *k) {
    return k->q_len == 0 && k->n == 0;
}

#endif /* PICO_ACE_KEYMATRIX_H */
