# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A **Jupiter Ace emulator for the ClockworkPi PicoCalc**, in C against the
Raspberry Pi Pico SDK. The guest is a Z80A at 3.25 MHz with an 8 KiB Forth
ROM, a 32×24 character display from 768 bytes of screen RAM and 1 KiB of
character RAM, a 40-key matrix, and a one-bit speaker and tape port.

**Status, 2026-10-05: all sixteen milestones (M0–M15) are done**
(`docs/design.md` §15). The record of each (what was verified, on which
board, on what date, and what was not checked) is in
`docs/milestones.md`. Add to it there for any later milestone. The
board used throughout is a Plus 2 W, id `7458DC82A89AAC12`, at 150 MHz.

**Settled decisions** (`docs/design.md` §18, 2026-10-03): the power-on
machine is the **19K** Ace; **the Ace ROM ships in the repository and is
embedded in the firmware**, under the permission in `roms/COPYING.md`; the host clock is **150 MHz** only, with 300 MHz deferred; `.ace`
snapshots are **imported, not exported**, and wait for M11, which also
chooses their reference emulator; the licence is **GPL-3.0**. Do not
reopen these without the owner.

## The documents

| File | Authority on |
|---|---|
| `docs/design.md` | the **guest** and the shape of this project's code: Ace hardware model, architecture, budgets, milestones, unverified constants |
| `docs/hardware-notes.md` | the **host**: PicoCalc wiring, protocols, timing, measured costs, quirks |
| `docs/emulator-lessons.md` | what pico-atom taught about writing any emulator on the PicoCalc |
| `docs/milestones.md` | what each milestone verified, on which board and date, and what it did not check |
| `docs/ace-sch-nocash.gif` | the Ace's circuit: Bodo Wenzel's schematic as nocash commented it (`THIRD-PARTY.md`) |

Read `docs/design.md` before writing emulator code. Its §4.6 says which
pico-atom files to reuse, §15 says what each milestone builds and when it is
done, and §16 lists every guest fact that is not yet confirmed.

**Cross-references are load-bearing.** `design.md` writes `HW §N` for
hardware-notes, `EL §N` for emulator-lessons and plain `§N` for itself. Keep
the section numbers accurate when editing. `hardware-notes.md` and
`emulator-lessons.md` are **portable**: they came from pico-atom and travel to
other projects, so they may cite each other but never `design.md`, this file
or a source file. New facts about the PicoCalc belong in `hardware-notes.md`,
written so that they stand alone.

**Notation:** `$XXXX` is a guest (Ace) address or value, in documents and
comments. `0x` is a host value, and is what C code uses. `T` is a Z80
T-state.

Documents are written in British spelling and in the plain, measured style of
the existing three: say what was checked, on what, and when.

## pico-atom: the sibling project

`../pico-atom` is an Acorn Atom emulator for the same hardware, by the same
author, under the same licence (GPL-3.0). Its drivers and a good part of its
core were verified on a Plus 2 W, and `design.md` §4.6 says which files to
**copy and rename**, which to **adapt**, and which are not used.

- **Read pico-atom, never edit it** from this project.
- Rename `PICO_ATOM_` → `PICO_ACE_`, `ATOM_` → `ACE_` and `atom_` → `ace_`.
  Change the `design.md §N` references in comments to this project's sections.
- **Bring its tests with it.** A copied module counts as reused only once its
  pico-atom tests pass here under the new names.
- A copied file brings its `THIRD-PARTY.md` entry: ClockworkPi's LCD init
  values (`lcd.c`) and FatFs (`ffconf.h`, with FatFs copied from the SDK at
  configure time and not kept in the tree).
- Its `CLAUDE.md` and `docs/design.md` §6.3 hold the measured numbers this
  design calibrates against: 158–218 host cycles per 6502 instruction, and
  86 % of core 0 at a 2 MHz guest with zero underruns.

## Build and test

As of M14 these all work, and `tools/uart-type.sh` types at the guest.

```sh
# host: src/core/ with the system compiler, no Pico SDK, under CTest
cmake -S . -B build/host -DPICO_ACE_HOST=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
tools/fetch-test-suites.sh           # ZEXDOC/ZEXALL and FUSE into test/suites
ctest --test-dir build/host --output-on-failure          # ZEX: ~90 s each in Debug
ctest --test-dir build/host --output-on-failure -LE long # without ZEX

# firmware: needs PICO_SDK_PATH and arm-none-eabi-gcc on PATH
cmake -S . -B build/pico -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build/pico -j          # -> build/pico/pico-ace.uf2, pico-ace-bench.uf2
tools/build.sh -DPICO_ACE_RAM_TIER=0 build/pico-t0      # another SRAM tier (2 ships), own dir
tools/build.sh -DPICO_ACE_HALT_SKIP=OFF build/pico-nohs # every HALT interpreted, a control
tools/build.sh -DPICO_ACE_UART=OFF build/pico-release   # the build that ships
tools/build.sh -DPICO_ACE_AUDIO=OFF build/pico-noaudio  # paced on the timer, a control
tools/build.sh -DPICO_ACE_TURBO=OFF build/pico-noturbo  # paced while a tape plays, a control
tools/build.sh -DPICO_ACE_WAIT=OFF build/pico-nowait    # no wait states, a control
tools/build.sh -DPICO_ACE_BOOT_RAM=3k build/pico-3k     # this machine over the card's
tools/build.sh -DPICO_ACE_BOOT_TAPE=SQ.tap build/pico-t # this tape in the deck at boot

# hardware, with the Debug Probe's SWD and UART both connected, and the
# Mac's display kept awake (caffeinate -d): a sleeping display wedges the
# probe until it is replugged (hardware-notes.md §2.7)
tools/uart-log.sh 30 out/run.log &   # capture UART1 first, so the banner is in it
tools/flash.sh                       # reset halt + resume, never reset run (HW §2.7)
tools/flash.sh build/pico/pico-ace-bench.elf   # M2's bench; embeds ZEXDOC if fetched
tools/uart-type.sh '2 2 + .\r'       # type at the guest over the same UART
tools/uart-screen.sh                 # the guest's screen, as text, into the log
tools/uart-hold.sh                   # park the guest and check the card; again to resume
tools/uart-type.sh '\x1e'             # RS opens the menu (US pauses); then the UART's bytes
tools/uart-type.sh '\x0e\r'           #   are its keys, ^P ^N ^B ^F the arrows, ESC closes
tools/perf-run.sh build/pico/pico-ace.elf out/perf    # design.md §14's workloads, a boot each
tools/perf-summary.sh out/perf                          #   one line per workload
tools/soak.sh build/pico/pico-ace.elf 30 out/soak       # §13.5's soak, on battery, then its check
PICO_ACE_TAP=game.tap build/host/test/host/test_tape  # an archive .tap through the trap
PICO_ACE_TAP=game.tap build/host/test/host/test_cassette  # ... and off the signal
PICO_ACE_TAP_OUT=sq.tap build/host/test/host/test_cassette  # the recorder's .tap, for xAce
PICO_ACE_ACE_DIR=dir build/host/test/host/test_snap_ace  # every archive .ace in dir
PICO_ACE_WAIT_ACE=g.ace PICO_ACE_WAIT_KEYS=$'GO\n' build/host/test/host/test_wait  # a game, held and not

# .ace against MAME (design.md §13.4): brew install mame, then
tools/mame/romset.sh                 # out/mame/roms; needs roms/JA-DOSROM/
tools/ace-reference.py dir --log out/m11-mame.log

# trace diff against xAce (design.md §13.4); CI runs the second line too
tools/trace/build-xace.sh            # clones xAce at a pinned commit into out/trace
tools/trace-diff.py run --keys '2 2 + .\n'
tools/trace-diff.py keys 'LOAD SQ\n7 SQ .\n' keys.txt   # a .tap through xAce's loader:
out/trace/xace-trace roms/ace.rom -f 1200 -k keys.txt -t sq.tap -s -q

# golden images (design.md §7.6): write them somewhere, look, then copy
mkdir -p out/golden && build/host/test/host/test_golden --write out/golden
```

Both targets build under `-Wall -Wextra -Werror`, and CI builds both on every
push. Run the host build as well as the firmware one: the core building clean
without the SDK is what keeps SDK headers out of `src/core/`.

- **Test suites are fetched, not committed.** `tools/fetch-test-suites.sh`
  puts ZEXDOC/ZEXALL and the FUSE Z80 tests (both GPL) into a gitignored
  directory. A missing binary makes its test exit 77, reported as skipped.
  **A skipped ZEXALL is an unverified CPU, not a pass.**
- **The Ace ROM is committed at `roms/ace.rom`** and is the only ROM the
  project ships. It is distributed under the 1998 Boldfield permission in
  `roms/COPYING.md`, **not under the GPL**. Keep `COPYING.md` and its
  `THIRD-PARTY.md` entry with it. The build embeds the ROM and fails if its
  SHA-1 is not `597ba8a15a292688333c84dc9fd35172abe5e7e6` (design.md §10.2).
  Host tests use the same file and never skip for want of it. Do not modify
  or replace it.
- **`roms/JA-DOSROM/` is not distributable and stays out of git.** No
  permission covers the Ace DOS ROM, and this design has no disc.
- **Tests use no framework**: `test/host/test_util.h` with `CHECK` and
  `TEST_DONE`, one binary per area. Assert behaviour by executing it rather
  than by comparing two tables in the repository, and give a timing test a
  control that must fail.
- **A golden image proves nothing until someone has looked at it.** View
  every changed PPM before committing it.

## Architecture: the parts that are easy to break

- **`src/core/` never includes an SDK header and never allocates.** State
  lives in `ace_t` or in buffers sized in `src/core/config.h`, where every
  fixed capacity lives. `src/port/` is the only place SDK headers belong.
- **`ace_run` is the seam.** It runs whole instructions until at least the
  requested T-states have passed and returns the true count. The caller
  carries the overshoot as debt. Never add a "run exactly N" variant.
- **Copy a machine with `ace_copy`, never `=`.** The page table points into
  the struct.
- **`page_t` is exactly two pointers** (`read`, `write`, NULL for the slow
  path). Per-page flags go in a separate array. The Ace's mirrors are done by
  the page table, and character RAM is write-only: its read pointer is NULL.
- **Core 0 owns the Z80, the machine, the speaker and the audio IRQ. Core 1
  owns the LCD, I²C, the SD card and the menu.** Core 1 sees the guest only
  through an immutable snapshot (screen, character set, status bytes) and
  never reads guest RAM while the Z80 runs.
- **There is no framebuffer.** The image is a function of 768 screen bytes
  and 1,024 character-set bytes. The renderer and its LUT live on core 1, not
  in `ace_t`.
- **The character set belongs in the presenter's shadow.** A program that
  redefines a character changes every cell showing it while the screen
  bytes stay identical. The dirty diff marks cells by changed glyph as well
  as changed byte (§7.3). A screen-only diff leaves stale glyphs for ever.
- **`keymatrix_field` owns `ace_t.keys`** and rewrites it every field.
  Host tests type through it (`guest_type`, `guest_press`) with PicoCalc
  codes; only a test that sweeps the ROM's own map writes cells directly.
- **Every even-port access moves the speaker**: `IN` one way, `OUT` the
  other, including the `IN`s that only read the keyboard (§8). Do not filter
  out "keyboard-only" reads.
- **INT is a level with a duration, not a point** (§5.3). The field split
  stops a slice at both of its edges.

## Hardware invariants

From `hardware-notes.md` §10. Each one cost real debugging time on pico-atom:

- `spi_set_format()` and the `D/CX` write go **before** CS low (the 40 ns
  CS-high rule).
- Never touch the LCD from an interrupt handler, and never mask interrupts
  around a blit. Audio has a ~3.5 ms refill deadline.
- Audio DMA ring: a power of two **and aligned**, with the hardware read
  wrap. On re-arm, reset **both** the read address and the transfer count.
  `DMA_IRQ_0` at priority `0x40`, and the refill path in SRAM.
- **Core 1 never calls `sleep_us`/`sleep_ms`**: their alarm IRQ runs on
  core 0, in the middle of the guest. Use `busy_wait_us_32` (HW §9.7).
- **Core 0 never calls `printf` once the guest runs.** It logs into a ring
  that core 1 drains.
- Poll the keyboard from core 1's loop in thread context. Build held-key
  state from press/release events. Shift+arrows, Shift+Space and
  Shift+Backspace never arrive (HW §6.3).
- Count PCM underruns **separately** from late DMA refills.
- No flash writes: settings go to a text file on the card, written through
  `.new` and a rename.
- Re-apply the SPI baud rate and the audio carrier after any `clk_sys`
  change. The host clock is a fixed 150 MHz (design.md §18), but `main()` still sets the core rail explicitly, because it survives a reset (HW §3).
- Build each SRAM tier in its own build directory and check its symbols with
  `arm-none-eabi-nm` (HW §9.8).

## Unverified constants

`docs/design.md` §16 lists every Ace fact written from secondary knowledge,
with a confidence for each. **Do not treat the design as authoritative for a
low or medium row.** Settle the row from a primary source, preferably by
**executing the real ROM** on the host harness (the keyboard matrix, tape
routines, key timing, RAM sizing, `HALT` use). Record in §16 how and when it
was settled, then make it a `#define`. A timing constant stays runtime
configuration until it is settled.

## Measurement discipline

Every performance figure in `design.md` is an estimate until a measurement
replaces it. Keep the estimate next to the measurement. When measuring: use
the mode that ships (paced on audio, core 1 presenting), carry the control
quantity (samples consumed per second, nominally 36,621 Hz), expect ~2 %
run-to-run spread, compare on one board, measure each feature against a
control build in the same sitting, and write results to a file. "Built" and
"done" are different words: a milestone with a device step is done only once
it has been checked on the device.

## Comments

Comments cite the document next to any decision that looks arbitrary, e.g.
`(design.md §7.3)` or `(hardware-notes.md §4.3)`, rather than restating the
reasoning. A device hook that does not exist yet is marked where its call
belongs, with a comment naming the milestone (`/* M13: ... */`).
