#!/bin/sh
# Fetch the Z80 test suites the host tests use (design.md §5.4).
#
# Both suites are GPL and neither is committed: the tree ships no
# binaries it did not build. Without them test_z80_zex and test_z80_fuse
# report as skipped, and a skipped ZEXALL is an unverified CPU, not a
# pass.
#
#   tools/fetch-test-suites.sh [dir]         # default: test/suites
#   PICO_ACE_TEST_SUITES=dir ctest --test-dir build/host
set -eu

dir="${1:-$(dirname "$0")/../test/suites}"
mkdir -p "$dir"

fetch() {
    echo "fetching $2"
    curl -sSLf -o "$dir/$2.part" "$1"
    mv "$dir/$2.part" "$dir/$2"
}

# Frank Cringle's exercisers, 1994, as distributed with z80emu.
zex="https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles"
fetch "$zex/zexdoc.com" zexdoc.com
fetch "$zex/zexall.com" zexall.com

# FUSE's per-opcode tests, from the emulator's own repository.
fuse="https://sourceforge.net/p/fuse-emulator/fuse/ci/master/tree/z80/tests"
fetch "$fuse/tests.in?format=raw" tests.in
fetch "$fuse/tests.expected?format=raw" tests.expected

cat <<NOTE

Fetched into $dir:
  zexdoc.com, zexall.com   CP/M programs, load and start at \$0100; they
                           print through BDOS functions 2 and 9 and end
                           with a jump to \$0000.
  tests.in, tests.expected FUSE's Z80 tests: one block per opcode, run
                           for at least the given T-states.
NOTE
