#!/usr/bin/env bash
# uart-type.sh — type at the PicoCalc over UART1 (design.md §9.1, §15.2 M6).
#
#   tools/uart-type.sh '2 2 + .\r'
#   tools/uart-type.sh $': sq dup * ;\r5 sq .\r'
#
# The firmware turns each received byte into the PicoCalc key events for
# it (keymap_picocalc_text, called from uart_keys in src/port/core0.c), so a
# hardware run can be driven from the machine capturing it. Characters type
# as themselves, the shifted ones inside Shift as the PicoCalc sends them;
# CR and LF are Enter, BS and DEL Backspace, ESC is Esc (BREAK), and the
# other control characters are Ctrl chords. The events go through
# keymatrix to the guest, as the keyboard's do; tools/uart-screen.sh reads
# the screen back.
#
# One character every 0.25 s: the replay takes 6 fields a key, 0.12 s
# (config.h, ACE_KEY_MIN_FIELDS + ACE_KEY_GAP_FIELDS), and a faster sender
# fills the UART's 32-byte FIFO and loses characters. UART_TYPE_DELAY
# overrides it.
#
# Writing while uart-log.sh reads is fine: that script refuses a second
# *reader*; this only writes. The port's settings are the ones uart-log.sh
# applied, so start the capture first.
set -euo pipefail

text="${1:?usage: uart-type.sh TEXT}"
delay="${UART_TYPE_DELAY:-0.25}"

dev="${UART_DEV:-}"
if [ -z "$dev" ]; then
    shopt -s nullglob
    cands=(/dev/cu.usbmodem* /dev/ttyACM*)
    if [ ${#cands[@]} -ne 1 ]; then
        echo "uart-type.sh: found ${#cands[@]} candidate ports (${cands[*]:-none}); set UART_DEV" >&2
        exit 1
    fi
    dev="${cands[0]}"
fi

# printf interprets \r and \n in TEXT, so a caller can write either.
text="$(printf '%b' "$text"; printf x)"
text="${text%x}"

exec 4>"$dev"
for ((i = 0; i < ${#text}; i++)); do
    printf '%s' "${text:$i:1}" >&4
    sleep "$delay"
done
exec 4>&-
