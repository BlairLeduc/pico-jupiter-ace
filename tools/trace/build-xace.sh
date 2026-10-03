#!/bin/sh
# Build xace-trace, the reference half of the trace diff (design.md
# §13.4), from xAce's own repository. Nothing of xAce is copied into this
# tree: it is cloned at a pinned commit into the build directory, its CPU
# and tape code are compiled there with one line added (the trace hook at
# the top of the CPU loop), and linked with xace-trace.c in place of its
# X11 front end.
#
#   tools/trace/build-xace.sh [OUT]          # OUT: out/trace
#   XACE_DIR=~/src/xAce tools/trace/build-xace.sh   # an existing checkout
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/../../out/trace}"
cc="${CC:-cc}"

# xAce 0.5 as Lawrence Woodman maintains it, GPL-2.0-or-later.
repo="https://github.com/lawrencewoodman/xAce.git"
commit="52d89b2"

mkdir -p "$out/obj"
src="${XACE_DIR:-$out/xAce}"
if [ ! -f "$src/src/z80.c" ]; then
    git clone -q "$repo" "$src"
    git -C "$src" checkout -q "$commit"
fi
[ -f "$src/src/z80ops.c" ] || { echo "$src: not an xAce checkout" >&2; exit 1; }

# The hook goes where the registers are mainloop's locals, before an
# instruction starts. Exactly one insertion, or the build stops.
sed 's/^\( *\)ixoriy=new_ixoriy;/\1XACE_TRACE(); ixoriy=new_ixoriy;/' \
    "$src/src/z80.c" > "$out/obj/z80-traced.c"
[ "$(grep -c 'XACE_TRACE();' "$out/obj/z80-traced.c")" = 1 ] ||
    { echo "build-xace.sh: the hook did not go in once; has z80.c changed?" >&2; exit 1; }

# xAce is old C. Its warnings are not ours to fix, so they are off for
# its files and on for the driver.
"$cc" -O2 -w -I "$src/src" -include "$here/xace-hook.h" \
    -c "$out/obj/z80-traced.c" -o "$out/obj/z80.o"
"$cc" -O2 -w -I "$src/src" -c "$src/src/tape.c" -o "$out/obj/tape.o"
"$cc" -O2 -Wall -Wextra -I "$src/src" -I "$here" \
    -c "$here/xace-trace.c" -o "$out/obj/xace-trace.o"
"$cc" -o "$out/xace-trace" "$out/obj/z80.o" "$out/obj/tape.o" "$out/obj/xace-trace.o"
echo "$out/xace-trace"
