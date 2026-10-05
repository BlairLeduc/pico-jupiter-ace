#!/usr/bin/env bash
# perf-run.sh — design.md §14's workloads on the board, one boot each
# (§15.2 M12).
#
#   tools/perf-run.sh build/pico/pico-ace.elf out/m12/t0-a        # every workload
#   tools/perf-run.sh build/pico/pico-ace.elf out/m12/t0-a idle scroll
#
# Each workload is its own boot: capture UART1, flash, type a Forth word
# at the guest over the same UART and start it, let it run, keep the
# log. The heartbeat's `perf` line is the measurement; perf-summary.sh
# reduces the logs. No card is needed.
#
# Measured in the mode that ships: audio paces the guest and core 1
# presents while it runs. The consumed rate on the heartbeat's `audio`
# line is the control quantity. Every program was run on the host first
# (test/host's guest harness) and ends in `0 until`, so it never
# returns to the prompt.
set -euo pipefail

elf="${1:?usage: perf-run.sh ELF OUTDIR [WORKLOAD...]}"
outdir="${2:?usage: perf-run.sh ELF OUTDIR [WORKLOAD...]}"
shift 2
workloads=("$@")
[ ${#workloads[@]} -gt 0 ] || workloads=(idle compute scroll sound glyphs)

here="$(cd "$(dirname "$0")" && pwd)"
dwell="${PERF_DWELL:-45}"   # seconds each program runs: ~9 heartbeats

# One line per argument, each typed and entered on its own: the ROM loses
# keys that arrive while it handles the line before (found on the host).
# glyphs rewrites the space's eight rows ($2D00, character RAM at $2C00)
# 256 times over, so every blank cell changes glyph (design.md §7.3).
program() {
    case "$1" in
        idle)    ;;
        compute) printf '%s\n' ': c 0 30000 0 do i + loop drop ;' ': r begin c 0 until ;' r ;;
        scroll)  printf '%s\n' ': v begin vlist 0 until ;' v ;;
        sound)   printf '%s\n' ': b begin 100 200 beep 0 until ;' b ;;
        glyphs)  printf '%s\n' ': g begin 256 0 do 8 0 do j 11520 i + c! loop loop 0 until ;' g ;;
        *) echo "perf-run.sh: unknown workload $1" >&2; exit 2 ;;
    esac
}

mkdir -p "$outdir"

# A failed flash or type exits under set -e; the capture must not outlive
# it, or it holds the port and every later run finds it busy.
logger=
stop_logger() {
    [ -n "$logger" ] || return 0
    kill "$logger" 2>/dev/null || true
    wait "$logger" 2>/dev/null || true
    logger=
}
trap stop_logger EXIT

for w in "${workloads[@]}"; do
    program "$w" >/dev/null           # an unknown name fails before the flash
    log="$outdir/$w.log"
    # The last capture's reader can outlive its kill by a moment, and
    # uart-log.sh refuses a busy port: retry until one is running.
    rm -f "$log"
    for try in 1 2 3 4 5 6 7 8 9 10; do
        "$here/uart-log.sh" 0 "$log" 2>/dev/null &
        logger=$!
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            sleep 0.5
            [ -e "$log" ] && break
            kill -0 "$logger" 2>/dev/null || break
        done
        if kill -0 "$logger" 2>/dev/null && [ -e "$log" ]; then break; fi
        kill "$logger" 2>/dev/null || true
        wait "$logger" 2>/dev/null || true
        logger=
        [ "$try" -lt 10 ] || { echo "perf-run.sh: no capture for $w" >&2; exit 1; }
    done
    "$here/flash.sh" "$elf" >"$outdir/$w.flash.log" 2>&1
    sleep 4                           # boot to the prompt, with or without a card
    while IFS= read -r line; do
        "$here/uart-type.sh" "$line\r"
        sleep 1
    done < <(program "$w")
    # Mark where the program started: heartbeats before this are typing.
    grep -ac ' perf ' "$log" >"$outdir/$w.start" || true
    sleep "$dwell"
    "$here/uart-screen.sh" >/dev/null 2>&1 || true
    sleep 1
    stop_logger
    echo "perf-run.sh: $w -> $log"
done
