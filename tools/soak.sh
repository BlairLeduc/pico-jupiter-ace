#!/usr/bin/env bash
# soak.sh — design.md §13.5's soak: 30 minutes on battery with a Forth
# program that prints, beeps and reads the matrix itself while keys are
# typed over the UART, UART1 captured throughout, then soak-check.py
# over the log (§15.2 M12).
#
#   tools/soak.sh build/pico/pico-ace.elf              # 30 minutes
#   tools/soak.sh build/pico/pico-ace.elf 45 out/soak
#
# Before running: the Debug Probe's SWD and UART connected, the Mac's
# display kept awake (caffeinate -d, hardware-notes.md §2.7), the USB-C
# power lead out so the PicoCalc runs on its batteries, and the power
# switch on. soak-check.py fails a run whose gauge shows charging, which
# is USB power; one that never shows it may still be USB with the charge
# finished, so say which when recording it.
#
# The program counts, prints the count and the half-row ENTER L K J H as
# Forth reads it from port $BFFE (design.md §2.4: 16 is H, 8 is J), and
# beeps, for ever; the screen scrolls a line each pass. This script
# types h and j at it every few seconds through the firmware's key path
# (keymatrix), and asks for the screen now and then, so the log shows
# the program reading them. Run on the host first with the guest
# harness: each typed key shows on a row. The southbridge is polled for
# the real keyboard all the while, which is what the I2C error count
# covers; press a few keys on it during the run as well, but not Shift
# with Space, which is BREAK.
set -euo pipefail

elf="${1:?usage: soak.sh ELF [MINUTES] [OUTDIR]}"
minutes="${2:-30}"
outdir="${3:-out/soak}"
here="$(cd "$(dirname "$0")" && pwd)"
every="${SOAK_KEY_EVERY:-5}"     # seconds between typed keys

mkdir -p "$outdir"
stamp="$(date +%Y%m%d-%H%M%S)"
log="$outdir/soak-$stamp.log"

# One line at a time: the ROM loses keys that arrive while it handles
# the line before.
program=(
    ': k 49150 in 31 and 31 xor ;'
    ': s 0 begin 1+ dup . k . cr 100 30 beep 0 until ;'
    's'
)

logger=
stop_logger() {
    [ -n "$logger" ] || return 0
    kill "$logger" 2>/dev/null || true
    wait "$logger" 2>/dev/null || true
    logger=
}
trap stop_logger EXIT

# Capture first, so the banner is in the log (CLAUDE.md).
"$here/uart-log.sh" 0 "$log" &
logger=$!
for _ in $(seq 1 20); do
    sleep 0.5
    [ -e "$log" ] && break
    kill -0 "$logger" 2>/dev/null || { echo "soak.sh: the capture did not start" >&2; exit 1; }
done

"$here/flash.sh" "$elf" >"$outdir/soak-$stamp.flash.log" 2>&1
sleep 4                          # boot to the prompt, with or without a card
for line in "${program[@]}"; do
    "$here/uart-type.sh" "$line\r"
    sleep 1
done
start=$(date +%s)
echo "soak.sh: running $minutes minutes from $(date +%H:%M:%S) -> $log"

end=$((start + minutes * 60))
n=0
while [ "$(date +%s)" -lt "$end" ]; do
    sleep "$every"
    if [ $((n % 2)) -eq 0 ]; then "$here/uart-type.sh" 'h'; else "$here/uart-type.sh" 'j'; fi
    n=$((n + 1))
    # The screen about once a minute, just after a key, so the dump shows
    # it; every 11th key, an odd count, so the dumps follow h and j in turn.
    if [ $((n % 11)) -eq 0 ]; then
        sleep 0.3
        "$here/uart-screen.sh"
    fi
done
sleep 10                         # a last heartbeat after the last key
stop_logger

echo "soak.sh: $n keys typed"
"$here/soak-check.py" --minutes "$minutes" "$log" | tee "$outdir/soak-$stamp.txt"
