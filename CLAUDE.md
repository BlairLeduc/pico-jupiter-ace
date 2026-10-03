# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A **Jupiter Ace emulator for the ClockworkPi PicoCalc**, in C against the
Raspberry Pi Pico SDK. The guest is a Z80A at 3.25 MHz with an 8 KiB Forth
ROM, a 32×24 character display from 768 bytes of screen RAM and 1 KiB of
character RAM, a 40-key matrix, and a one-bit speaker and tape port.

**Implementation status, 2026-10-03: M3 done.**
The next milestones are M4 and M5 on the host and M6, board bring-up
(`docs/design.md` §15).

**M3, the Ace on the host** (`src/core/ace.c`, `test/host/guest.c`,
`tools/trace/`), done 2026-10-03 on the workstation (Apple M1 Pro, Apple
clang 21). The real ROM boots to its cursor in the 3K, 19K and 51K machines
and runs `2 2 + .` to `4  OK`; 88,192 T from power-on to the prompt in the
19K. Bus, field and ROM-embedding tests pass with their controls. The trace
diff against xAce (cloned at `52d89b2`, built headless) is clean line for
line through boot and a typed line, with xAce's errata named in
`tools/trace-diff.py` and `xace-trace.c`; xAce loaded an archive `.tap`.
xAce has no `.ace` loader; the owner moved that check to M11 (design.md
§18 item 6). CI green on both jobs for PR #2, 2026-10-03, with the trace
diff run there too. **Not checked:**
the field's line numbers and INT length against the schematic (they are
MAME's, as runtime configuration); character-RAM and open-bus read values;
anything on the device. The Ace powers on to a blank screen with a cursor,
not to `OK`.

**M2, the Z80 on the board** (`src/bench/`, `src/port/bench_main.c`), on
the Plus 2 W (id `7458DC82A89AAC12`) at 150 MHz, gcc 15.2 `-O3`,
2026-10-03: the Forth-shaped loop ran at 81.8 host cycles per instruction
from flash and 82.8 in SRAM (tier 2); ZEXDOC's first group at 103.4 and
86.0. Mean T per instruction was 7.82 and 8.09, and the board's T-state and
instruction counts equal `test_bench`'s on the host. Eight runs per image
agreed to within 0.01 % (`out/m2-bench-t0.log`, `out/m2-bench-t2.log`).
**Gate decision: 150 MHz is enough**, at a projected 23–28 % of core 0
(design.md §3.2). Tier 2 needed `rd`/`imm16`/`push16`/`pop16` marked
`ACE_HOT2`, because GCC kept out-of-line copies in flash (HW §9.8).
**Not checked:** the Ace's bus, interrupts and the real ROM's mix; core 1
sharing the XIP cache; CPU state in locals, which moved to M12 as optional.
CI green on both jobs for PR #1, 2026-10-03.

**M1, the Z80 on the host** (`src/core/z80.c`), checked on the workstation
(Apple M1 Pro, Apple clang 21), 2026-10-03, against suites fetched that day
by `tools/fetch-test-suites.sh`: all 1,356 FUSE tests pass on registers,
MEMPTR, T-states, memory and access order; ZEXDOC and ZEXALL each pass all
67 groups; `test_z80_behaviour` passes (interrupts, `HALT`, the run
contract, Q). ZEXALL wall time, a regression marker only: 90.6 s Debug,
26.4 s Release (46.7 G T-states). The harness was shown to fail with planted
bugs (design.md §5.4). Where sources disagree the CPU matches FUSE, and
§5.1 lists the four choices that follow. CI green on both jobs for
`633d1c0`, 2026-10-03, fetching the suites and running all five tests
(ZEXDOC and ZEXALL ~139 s each in its Debug build). **Not checked:** the tier-2
SRAM placement by symbol address, since no firmware links the Z80 until M2;
and the CPU state is not yet in locals (§5.2), which M2 measures.

**M0, the skeleton**, 2026-10-03 (SDK 2.3.1, arm-none-eabi-gcc
15.2.Rel1, Apple clang 21): both targets build under `-Werror`; CTest
passes; an `#include "pico/stdlib.h"` put in `src/core/config.h` fails the
host build; the test fails with `ACE_ROM_SIZE` set wrong. Image: 22,692 B
text, 852 B bss (UART build); 20,868 B, 836 B (no UART), with gcc 15.2;
CI's gcc 13.2 gives 22,292 B and 20,452 B of text. On a Plus 2 W (RP2350B,
rev 2, id `7458DC82A89AAC12`), 2026-10-03, flashed with `tools/flash.sh`
and captured with `uart-log.sh`: the banner reports clk_sys and clk_peri at
150 MHz and the core rail at ~1,100 mV, followed by 11 heartbeats a second
apart. CI green on both jobs for `471e8f1`, 2026-10-03. **Not checked:**
the LED. A `pico2` image's GP25 is the radio's CS on a W board (HW §1.1),
so the LED can only light on a Pico 2. `PICO_ACE_RAM_TIER` did not reach
the core library until M1 fixed it.
When a milestone is done, record it here: what was verified, on which
board, on what date, and what was not checked. pico-atom's `CLAUDE.md`
shows the form.

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

As of M3 these work, except `tools/uart-type.sh` (M6, which turns UART bytes
into key events).

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
tools/build.sh -DPICO_ACE_RAM_TIER=2 build/pico-t2      # an SRAM tier, in its own dir
tools/build.sh -DPICO_ACE_UART=OFF build/pico-release   # the build that ships

# hardware, with the Debug Probe's SWD and UART both connected
tools/uart-log.sh 30 out/run.log &   # capture UART1 first, so the banner is in it
tools/flash.sh                       # reset halt + resume, never reset run (HW §2.7)
tools/flash.sh build/pico/pico-ace-bench.elf   # M2's bench; embeds ZEXDOC if fetched
tools/uart-type.sh '2 2 + .\r'       # type at the guest over the same UART

# trace diff against xAce (design.md §13.4); CI runs the second line too
tools/trace/build-xace.sh            # clones xAce at a pinned commit into out/trace
tools/trace-diff.py run --keys '2 2 + .\n'
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
