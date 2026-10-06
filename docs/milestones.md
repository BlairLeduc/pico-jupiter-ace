# Milestone log

What each milestone verified, on which board, on what date, and what was
not checked, newest first. `design.md` §15.2 holds each milestone's scope
and done-when criteria; this file keeps the full record, moved here from
`CLAUDE.md` on 2026-10-06.

**M15, finish** (`src/core/keylayout.c`, `status.c`, `keymap_picocalc.c`,
`settings.c`; `src/port/menu.c`, `keymapio.c`, `display.c`, `tapeio.c`;
`tools/swd-counters.py`; `README.md`), on the Plus 2 W (id
`7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-05. Game layouts are
pico-atom's parser and overlay with the Ace's targets: built-ins CURSOR
(arrows as 5 6 7 8, `]` as 0) and QAOP (O Q A P, `]` as SPACE), card
layouts in `/ace/keymaps/`, and a `tapes` line that names **files**
(`.tap` or `.ace` stems), since an `.ace` has no name. `test_keyboard`
types each built-in through the ROM (`56780`, `oqap `), the standard
map's arrows the control; layout lookup turned off fails 7 checks. **The
owner decided the menu is pico-atom's** (2026-10-05): its items less
Discs, its rows and Escape, F1 Tapes, F3 Snapshots, F4 Setup (the layout
is its Keys row), F5 Machine, F10 About, Alt+H keys, Alt+K reset (Alt+R
gone); F2, pico-atom's Discs, is nothing. `status.c` came from pico-atom
with `test_status`: the perf line at the panel's top, the tape's status
line at its foot, each hidden by Setup or the file's new `status`;
`backlight` joined the file. Tapes gained New tape (`TAPEnn.tap`, empty,
in the deck); pico-atom's Record is left out, as the Ace's recorder
follows the ROM's save cue. The build that ships has no UART, so core 0
copies its counters into `g_swd` once a second and
`tools/swd-counters.py` reads them with OpenOCD's `read_memory`, no halt:
30 reads in 44 s left underruns and late refills at 0 (HW §2.7). **The
release soak passed** on battery, 30 minutes, the program typed on the
PicoCalc and H and J held by hand: rt 1.000 in every 10 s window, every
failure counter 0, 36,618–36,624 Hz, core 0 20.3–21.8 %, the count
rising in 180 dumps, 45 real key events; **die 20 °C throughout**,
uncalibrated (`out/m15/soak/`). A 1-minute run with no program fails it,
the control. The owner checked every page on the panel, every menu key,
every setting, pause and the backlight; saved settings, rebooted, and the
changed settings were used; saved and loaded words. Over the UART:
Machine 51K to 3K (`16384 c@ .` 255) and back (252), CURSOR's `]` typed
`0`, New tape made `TAPE01.tap` and a SAVE wrote to it. CI green on both
jobs for PR #14. **The final build's soak** (after the menu rework, 30
minutes on battery, `out/m15/soak2/`): one boot, rt 1.000 in every
window, every failure counter 0, the count rising in all 180 dumps,
36,621.1 Hz over the run, core 0 20.4–22.0 %, die 20–21 °C. Its check
**failed on the workload's keys alone**: the dumps showed H read 18
times and J never, and the owner chose to record it as it stands
rather than run it again. The same run showed two windows 1/10 fast and
slow, a read torn while core 0 rewrote the block; `swd-counters.py` now
reads the block twice and keeps a read only when both agree (not yet
used for a whole soak). The owner loaded a word back off a tape
made by New tape, on the Plus 2 W. **Not checked:** J read by the
program in the final build's soak. The owner
read the settings file on a computer after saving and found it right,
and made `/ace/keymaps/invaders.map`, chose it on the Keys row and found
its keys remapped. A test SAVE went onto the owner's `BIG.tap` by
mistake (40 bytes appended); the owner truncated it back.

**M14, wait states** (`src/core/ace.c`; `test/host/test_wait.c`;
`src/port/main.c`, `core0.c`), on the Plus 2 W (id `7458DC82A89AAC12`)
at 150 MHz, gcc 15.2, 2026-10-05. Read from the schematic (Wenzel's,
commented by Martin Korth; the Mercury Ace clone's equations agree): the
Z80's clock is the pixel counter's CNT0, and a memory access to `$2400`
or `$2C00` in the first 128 T of display lines 0–191 is held to T 128.
The same reading put the display at the circuit's line 0, 64 lines after
INT, not MAME's 56. **Modelled, on by default** (`PICO_ACE_WAIT=OFF` is
the control; `ace-trace` turns it off for xAce). The schematic, Wenzel's
as nocash commented it, is `docs/ace-sch-nocash.gif`; from it §16 also
settled the clock, the field's shape and INT (now `#define`s), the port
decode, the speaker's polarity and the memory decode, and found the
data bus without pull-ups, so open bus is the video's fetch, not `$FF`
(not modelled). Host: the ROM printing
a screenful takes 15.9 % longer, dreamsoft racer runs 3.8 % fewer
instructions a field, `VLIST` does not move; taking each access 4–11 T
into its instruction instead of at its start moved nothing that matters.
Board, against the control in one sitting: core 0 within 0.5 points on
§14's workloads and a print loop, 0 underruns; the print loop held 29.5 %
of the time (`out/m14/`). **Not checked:** timing against a real Ace;
the shipping build.

**M13, signal-level tape** (`src/core/cassette.c`, `tape.c`, `ace.c`;
`src/port/tapeio.c`, `menu.c`, `core0.c`), on the Plus 2 W (id
`7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-05. The player gives
the half-cycles the ROM's save routine counts (design.md §10.4's table),
and `test_cassette` holds it to the ROM's recorded `SAVE` edge for edge,
9,924 edges and the gap between header and data. The ROM's `LOAD` and
`VERIFY` read it with the trap off; the recorder and an independent
decoder in the test both turn D3 into the `.tap` the trap writes; the
access line decodes to nothing, which settled §16: the tape output is
D3. Six planted bugs each fail the test. `tut-tut.tap` loads off the
signal on the host in 67.0 s of guest time. `fast_tape = off` (Settings
page, or the file) keeps the trap's stall as the port's cue, loads the
`.tap` whole into a 64 KiB image and declines, so the ROM reads the
signal. On the board, in the 51K machine: a word `SAVE`d at signal level
went to `/ace/tapes/M13CU.tap`, loaded in xAce (`3 M13CU . 27  OK`, from
the bytes the log dumps), and loaded back on the board off the signal.
`LOAD TUTTUT` off the signal: turbo held 3.18–3.19× real time, 3,356
fields in 23.1 s of wall time against 69.9 s paced; 0 underruns and 0
late refills either way. Core 0 while a tape plays, paced
(`PICO_ACE_TURBO=OFF`): 30.7–31.6 %, against 20.9 % at the prompt
(`out/m13/device2.log`). With no tape playing, §14's five workloads
read within 0.3 points of M12's build in the same sitting (compute
23.3 % both). Unpaced, core 1 cannot present every field, and
dropped 12 snapshots across a save. Found on the way: with the deck
empty, a LOAD of a name the card lacks logged the last file the header
search had looked at (M10's; only the message was wrong). After Codex's review the recorder keeps only whole
blocks (`test_cassette`, with a control), and the port saves by the trap
when the 64 KiB image is full, empties the scratch at each header, and
keeps a recording when the card is missing until the card changes. On
the board: two signal-level SAVEs to separate files with the scratch
emptied between them, and a 65,534-byte BSAVE whose data the trap saved
for want of room (`out/m13/device3.log`); the card-missing case was not
checked on the board. **Not
checked:** Play by hand from the menu on the board (host only); a card
pulled while a recording waits to be written; a recording onto a tape the
user put in the deck (the save went to a new file).

**M12, the performance pass and soak** (`src/core/z80.c`, `hot.h`;
`tools/perf-run.sh`, `perf-summary.sh`, `soak.sh`, `soak-check.py`), on
the Plus 2 W (id `7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-04.
Five Forth workloads over the UART, a boot each, every change against a
control in the same sitting (design.md §3.2). Kept: `HALT` fast-forward,
in the `HALT` opcode, scrolling 41.9 % of core 0 to 3.4 %; **tier 2, now
the CMake default**, compute 31.8 % to 23.3 %. Not kept: the ROM in SRAM
(nothing at tier 2). Not tried: computed `goto`, CPU state in locals. The
heaviest workload is compute at 23.3 %, against M11's shipped 57.7 %
(scrolling). A per-instruction check in `z80_run` had cost 23 %: it
stopped GCC inlining the step; the step is now `always_inline` and the
loop checks nothing new. At tier 0 the layout moves results by up to 9
points between builds; tier 2 repeats to 0.1. The 30-minute soak passed
(`out/m12/soak/soak-20261004-231251.log`): one boot, rt ≥ 0.999, every
failure counter 0, both typed keys read by the program; on battery
throughout, by the owner's word, 2026-10-05 (gauge 90 % to 89 %, never
charging). The owner ran the shipping build (`PICO_ACE_UART=OFF`, tier
2) on a Pico 2 W, 2026-10-05: it boots, types, scrolls, beeps and loaded
a snapshot. **Not checked:** keys pressed on the PicoCalc during the
soak; the shipping build's figures, which it does not log. A build
directory from before M12 keeps its cached tier 0; pass
`-DPICO_ACE_RAM_TIER=2` once.

**M11, snapshots** (`src/core/snap_ace.c`, `snapshot.c`, `sha1.c`;
`src/port/snapio.c`, `menu.c`), 2026-10-04. The `.ace` format was settled
from the archive's FAQ, MAME's loader and 199 archive files (design.md
§10.5, §16): no file dumps past `$7FFF`, so 35K and 51K files lack their
stack, which in the ROM's key wait is the one word `$04F7` at RAMTOP − 2
(135 files). The owner chose **MAME, run headless, as the reference**, and
**35K files into the 51K machine** with that word written back (§18 items
6 and 7). On the host 198 of the 199 load and Ace Invaders is refused;
MAME agrees on all 102 19K files saved in the key wait
(`tools/ace-reference.py`). `.sav` is pico-atom's format with the Z80's
fields; a restored machine meets the original 150 fields on. On the Plus
2 W (id `7458DC82A89AAC12`): a word saved to slot 1, forgotten, and loaded
back ran; save 132.0 ms, load 47.1 ms, 0 underruns. The owner ran the
shipping build on a Pico 2 W, 2026-10-04: Pacman and Othello loaded in
the 19K, which refused the rest; booted as 3K and 51K (`ram =` in the
settings file, as the menu cannot change RAM until M15), every other file
loaded, the 35K ones into the 51K, and Ace Invaders was refused. On the
Plus 2 W the 35K files load into the 51K in 19.8–58.0 ms, and refusals
take 4.5–7.9 ms (`out/m11-ace.log`). **Not checked:** a card pulled
mid-save. After
the first flash the Debug Probe dropped off USB until replugged, as once in
M10: the Mac's display sleeping wedges it (HW §2.7; `caffeinate -d`, not
`-s`).

**M10, the menu and fast tape** (`src/core/tape.c`, `romfont.c`;
`src/port/tapeio.c`, `menu.c`, `textpage.c`, `park.c`), on the Plus 2 W
(id `7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-04. The trap is on
the ROM's block routines (`$1820`, `$18A7`) and hands back to the ROM's
own code for the last byte, so the checksum test, BREAK check, `EI` and
`RET` are the ROM's; `test_tape` holds it to the ROM's routines fed their
own recorded signal, every byte of RAM and every register but R. On the
board: a word SAVEd to the card loaded back after the owner's power cycle
(`3 CUBE .` printed 27); a LOAD from a tape in the deck passed another
program first; a LOAD no file answers fell through to the ROM; the
archive's `tut-tut.tap` loaded and ran in the 19K machine. A 16 KiB block
loads in a 41.9 ms park and saves in 94.4 ms, with 0 underruns through
all card work. The owner checked Alt+M, F1 and Alt+P on the panel; the
menu is in mixed case in the Ace's own character set, expanded from the
ROM and held by `test_boot` to what the ROM writes. The owner saved
settings on the board and they came back after a reboot, and ran the
shipping build on a Pico 2 W. The save had kept the refused line it was
given; the rewriter now writes over a refused value, or comments it out
beside a good line for the key (EL §8.7), and on the board a save then
cleared line 4's problem. **Not checked:** VERIFY on the board; a card
pulled mid-job. Once the UART went silent and the probe needed a replug:
the Mac's display had slept (found in M11, HW §2.7).

**M9, the card** (`src/port/sd.c`, `diskio.c`, `storage.c`, `card.c`,
`park.c`, `settingsio.c`; `src/core/settings.c`), on the Plus 2 W (id
`7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-04. pico-atom's SD and
FatFs layers and its settings parser, renamed; `test_settings` passes with
the Ace's keys. Core 1 reads `/ace/pico-ace.cfg` before core 0 powers the
machine on; only `ram` is applied yet. Boot to the prompt: 478.5 ms with
no card, 681.5 ms with a card lacking `/ace/` (mount 191.8 ms), 702.5 ms
with the file (mount 211.3 ms, read 9.7 ms); 505.5 ms once the card is
warm (mount 14.6 ms). `ram = 3k` boots a 3K machine (`$4000` reads 255;
the 19K control reads 1), and a bad line is skipped and named on every
heartbeat. `tools/uart-hold.sh` parks the guest (silence fed, card checked
on entry and on each change); the owner pulled and reinserted the card
while parked, the slot bounced out-in-out-in, one mount failed
`FR_NOT_READY`, nothing hung, and the guest resumed to `4  OK` with 0
underruns. **Not checked:** an empty card (none to hand); unmountable
cards; a card pulled mid-job; the card swap on the final build (the park
itself was rechecked without a card). The shipping build
(`PICO_ACE_UART=OFF`) was run by the owner on a Pico 2 W, 2026-10-04, and
works (no UART, so no board id, timings or park).

**M8, audio** (`src/core/beeper.c`, `src/port/audio.c`, `core0.c`), on
the Plus 2 W (id `7458DC82A89AAC12`) at 150 MHz, gcc 15.2, 2026-10-04.
pico-atom's beeper and audio, renamed; core 0 now paces on the audio
queue (`PICO_ACE_AUDIO=OFF` keeps the timer). `test_audio` runs the ROM's
`BEEP` (loop at `$0BAF`, half period 13m + 2 T, design.md §8): every half
period is the hand count at m = 50, 100 and 300, every sample matches an
independent box filter to 1 LSB, and the output's pitch is the count's to
1 in 10⁴. The prompt and typing make no edge: the ROM does not click. A
10-minute run read 36,621 Hz consumed (36,620 in 8 of 126 windows), 0
underrun samples, 0 late refills, no drops or I²C errors
(`out/m8-soak.log`); the board's `BEEP` edge counts equal the host's.
Audio costs core 0 0.4–2.0 points against a control build in the same
sitting (design.md §3.2). The owner heard `BEEP` on the PicoCalc's
speaker, 2026-10-04, and it sounds correct. CI green on both jobs for PR
#7, 2026-10-04. The shipping build (`PICO_ACE_UART=OFF`) was run by the
owner on a Pico 2 W, 2026-10-04, and its `BEEP` sounds correct (no UART,
so no board id or counters). **Not checked:** the shipping build's
counters, which it does not log.

**M7, the Ace on the device** (`src/port/core0.c`, `core1.c`,
`handoff.c`, `display.c`), on the Plus 2 W (id `7458DC82A89AAC12`) at
150 MHz, gcc 15.2; measured 2026-10-03, keyboard checked by the owner
2026-10-04. Core 0 paces `ace_run_field` on `time_us_64()` against an
absolute deadline; core 1 presents dirty bands at (32,64) and a grey perf
line below. On the PicoCalc keyboard: `2 2 + .` prints `4  OK`, the
arrows, DELETE and BREAK work, `VLIST` scrolls cleanly with the perf
line's drops at 0. Over the UART, `VLIST` repeated ran 4,000+ fields with
0 dropped snapshots at tiers 0 and 2, and a 10-minute idle run had no late
fields, drops or I²C errors. Core 0: idle 20.1 %; compute 36.4 % at
159.6 host cycles/insn (tier 0), 20.4 % at 89.4 (tier 2); scrolling
38.6 % / 36.2 %, its counts inflated by `VLIST`'s `HALT` (design.md
§3.2). Tier 1 gained nothing. Longest present 11.86 ms (the boot's full
redraw). `tools/uart-screen.sh` dumps screen RAM to the log. The
shipping build (`PICO_ACE_UART=OFF`) passed the same keyboard checks on a
Pico 2 W, run by the owner 2026-10-04 (no UART, so no board id).
**Not checked:** audio pacing (M8).

**M6, board bring-up** (`src/port/southbridge.c`, `lcd.c`, `kbd.c`,
`log.c`, `display.c`, `main.c`), on the Plus 2 W (id `7458DC82A89AAC12`,
RP2350B rev 2) at 150 MHz, gcc 15.2, firmware `e73791c`, 2026-10-03. The
drivers are pico-atom's, renamed. The test pattern was looked at on the
panel at 75 MHz SPI: grey panel-edge frame, white border on the 256×192
rectangle at (32,64), red, green, blue and yellow in the right corners.
Every key pressed was logged with its code and its release (Shift, Ctrl,
Alt, arrows, Esc, Break, F1–F5, F10, Insert by Shift+Enter and Alt+I);
modifiers send `held` while down. `tools/uart-type.sh` bytes arrive as
the events a press sends. A 655 s run: 19,651 polls, **0 I²C errors**, no
ring overflows or dropped log lines (`out/m6-soak.log`). Measured: blits
of 23.90–23.93 ms for 320×320 and 11.475 ms for 256×192 (wire and DMA
alone, ~9.5 % over the wire math); an I²C transaction 4,825–4,845 µs. The
captured Alt+I sequence, Alt let go first, is now a `test_keymap` case.
The southbridge reports version 0, as on pico-atom's board. CI green on
both jobs for PR #5, 2026-10-03. **Not checked:** the shipping build (`PICO_ACE_UART=OFF`) on the device, which
was only built; anything with the guest (M7).

**M5, the keyboard on the host** (`src/core/keymatrix.c`,
`keymap_picocalc.c`), done 2026-10-03 on the workstation (Apple M1 Pro,
Apple clang 21). The ROM sweep (40 cells alone, with SHIFT, SYMBOL SHIFT
and both) replaced design.md §2.4 and is kept in `test_keyboard`. It
corrected two beliefs: up is SHIFT+6 and down SHIFT+7, and SHIFT+3 types
`3` (no TRUE VIDEO; INVERSE VIDEO toggles). Every printable PicoCalc entry
types its character through `keymatrix` and the ROM; the editing keys, the
Alt layer and BREAK (`ERROR 3`) by their effect; `2 2 + .` through the
harness in all three machines. The ROM needs a key held 3 fields and 1 up
(4 fields a key), with controls at 2 held and 0 up that lose keys; the
replay uses 4 and 2, 6 fields a key. Planted bugs (arrows swapped, no
unshift, a 2-field hold) each fail a test. The ROM types SHIFT+SYMBOL
SHIFT+key as SYMBOL SHIFT+key, so `unshift` is for matrix fidelity, not
the ROM's text. CI green on both jobs for PR #4, 2026-10-03. **Not
checked:** real southbridge events (M6) and typing on the device (M7);
game layouts are M15.

**M4, video on the host** (`src/core/render.c`, `snappool.c`, `font.c`),
done 2026-10-03 on the workstation (Apple M1 Pro, Apple clang 21). Five
golden images in `test/host/golden/` (boot, inverse, redefined, every
glyph, the emulator's font) were looked at before committing, and a
one-pixel change fails `test_golden`. The glyph-change bands keep a
simulated panel exact over 300 random edits; the screen-only control goes
stale. The pool survives 100,000 random transitions. Zeroed character RAM
draws all paper at power-on, and the ROM writes the set within the first
field. The font is font8x8 (public domain) in `third_party/`, converted by
`tools/mkfont.py`. CI green on both jobs for PR #3, 2026-10-03.
**Not checked:** anything on the panel (M7).

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
