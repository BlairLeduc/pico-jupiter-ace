#!/bin/sh
# romset.sh — MAME's jupace ROM set in out/mame/roms, for
# tools/ace-reference.py (design.md §13.4, M11).
#
# MAME wants the Ace ROM as its two 4 KiB halves (design.md §10.2), the
# Deep Thought disc ROM and the SP0256-AL2's. The disc ROM is taken from
# roms/JA-DOSROM/, which is not distributed and stays out of git; the
# speech chip's is not to hand, and a zero-filled stand-in of its size
# lets MAME start (it warns of a wrong checksum). Neither is used by a
# snapshot load. Everything here is under out/, which git ignores.
set -eu
cd "$(dirname "$0")/../.."
R=out/mame/roms/jupace
mkdir -p "$R"
head -c 4096 roms/ace.rom > "$R/rom-a.z1"
tail -c 4096 roms/ace.rom > "$R/rom-b.z2"
if [ -f roms/JA-DOSROM/AceDos.bin ]; then
    cp roms/JA-DOSROM/AceDos.bin "$R/dos_4.bin"
else
    echo "romset.sh: no roms/JA-DOSROM/AceDos.bin; MAME will not start without dos_4.bin" >&2
fi
[ -f "$R/sp0256-al2.ic1" ] || head -c 2048 /dev/zero > "$R/sp0256-al2.ic1"
echo "romset.sh: $R"
