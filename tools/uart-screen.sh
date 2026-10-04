#!/usr/bin/env bash
# uart-screen.sh — ask the firmware for the guest's screen as text
# (design.md §15.2 M7).
#
#   tools/uart-log.sh 10 out/screen.log &
#   tools/uart-screen.sh
#
# Sends FS (0x1C), which no key sends; core 0 logs the 24 lines of screen
# RAM (dump_screen in src/port/core0.c), so the capture shows what the
# panel should. Start uart-log.sh first: this only writes.
set -euo pipefail
UART_TYPE_DELAY=0 exec "$(dirname "$0")/uart-type.sh" '\034'
