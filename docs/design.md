# Jupiter Ace for the PicoCalc: design

The design of an emulator of the **Jupiter Cantab Jupiter Ace** (1982) for the
**ClockworkPi PicoCalc**, in C against the Raspberry Pi Pico SDK. This is the
authority on the guest machine and on how the code is built. It covers the
hardware model, architecture, memory and CPU budgets, testing, milestones and
what is deliberately left out.

**Companion documents.**

| Reference | Document | Authority on |
|---|---|---|
| **HW §N** | `hardware-notes.md` | the host: PicoCalc wiring, protocols, timing, measured costs |
| **EL §N** | `emulator-lessons.md` | what the previous emulator taught: architecture, accuracy, testing, process |
| plain **§N** | this document | the Ace, and the shape of this project's code |
| **pico-atom** | `../pico-atom` (its `docs/design.md` §N) | the Acorn Atom emulator the lessons came from: verified code to reuse (§4.6), and measured numbers to calibrate against (§3.2) |

This document applies the lessons. It does not repeat them. Where a decision
just follows a lesson, it cites the lesson and moves on, and the space goes
to what is different about the Ace.

**Status, 2026-10-03.** M0 to M4 are done: the skeleton, the Z80 on the
host, the Z80 on the board, whose gate passed at 150 MHz (§3.2), the Ace
on the host, and its video on the host (§15.2). Beyond M0's banner, the only device measurement is M2's. Every number about the Ace below comes from secondary knowledge
until §16's table says otherwise. Every performance figure is an
**estimate** and is labelled as one (EL §14.4), except M2's measurements
of the Z80 alone, which §3.2 labels as measured.

**Notation.** Guest addresses and values use the Z80 world's `$XXXX`, and host
values use `0x` (EL §14.1). `T` is a Z80 T-state, one guest clock cycle at
3.25 MHz.

**Contents**

1. [Goals](#1-goals) ·
2. [The guest](#2-the-guest) ·
3. [Host target and budgets](#3-host-target-and-budgets) ·
4. [Architecture](#4-architecture) ·
5. [The Z80](#5-the-z80) ·
6. [Bus and memory](#6-bus-and-memory) ·
7. [Video](#7-video) ·
8. [Audio](#8-audio) ·
9. [Keyboard](#9-keyboard) ·
10. [Media and settings](#10-media-and-settings) ·
11. [Timing](#11-timing) ·
12. [User interface](#12-user-interface) ·
13. [Testing](#13-testing) ·
14. [Measuring](#14-measuring) ·
15. [Milestones](#15-milestones) ·
16. [Unverified constants](#16-unverified-constants) ·
17. [Dropped and deferred](#17-dropped-and-deferred) ·
18. [Decisions](#18-decisions) ·
19. [Sources](#19-sources)

---

## 1. Goals

**The product.** Turn the PicoCalc on and get the Ace's Forth `OK` prompt
within a second, on the panel, at real speed, with sound. Type Forth on the
PicoCalc keyboard. Load and save programs as files on the SD card. Run the
Ace's software archive (tapes and snapshots) unmodified.

**Accuracy target.** Cycle-correct at instruction granularity (EL §3.1): every
Z80 instruction takes its documented T-states, interrupts are taken where a
real Z80 would take them, and the 50 Hz interrupt and the speaker are timed
against the guest clock. Wait states on video memory accesses are a measured
follow-up, not a first-version requirement (§6.4).

**Non-goals for the first release**, each argued in §17: the Ace's add-on
hardware (sound board, printer, ROM packs), RP2040 boards, the 300 MHz host clock, `.ace` export, display snow,
scaling, colour themes.

**What makes the Ace a good fit.** It is a small machine. Its whole visible
state is 768 bytes of screen and 1 KiB of character RAM. It has no timer
chip, no video modes, no colour and no disc. Most of the risk is in one place,
the Z80 interpreter's cost (§3.2). Most of the work is in the keyboard, the
media formats and the ROM's contracts.

---

## 2. The guest

What we believe the Ace is. Every constant here is listed in §16 with its
source and confidence. It becomes a `#define` only once settled (EL §14.2).

### 2.1 Overview

| Part | Ace | Confidence |
|---|---|---|
| CPU | Z80A at **3.25 MHz** (6.5 MHz crystal ÷ 2) | high |
| ROM | **8 KiB** at `$0000–$1FFF`: the Forth system, character set source, tape routines | high |
| Video RAM | 1 KiB, 32×24 character cells (768 bytes used) | high |
| Character RAM | 1 KiB, 128 characters × 8 bytes, **written by the CPU, read only by the video circuit** | medium |
| User RAM | 1 KiB on the stock machine, mirrored | medium |
| Expansion | up to 48 KiB at `$4000–$FFFF` on the edge connector; a 16 KiB pack was the common one | medium |
| Display | 256×192 monochrome, white on black, bit 7 of a cell inverts it | medium-high |
| Interrupt | maskable `INT` once per field from vertical sync, IM 1 (`RST $38`) | medium |
| Keyboard | 40 keys, 8 half-rows × 5, read with `IN` on an even port, address high byte selects rows | medium |
| Sound and tape out | one bit: an `IN` from the port drives it one way, an `OUT` the other | medium |
| Tape in | one bit on the same port read | low |
| Field | 50 Hz PAL, 312 lines × 208 T = 64,896 T | medium |

### 2.2 Memory map

| Range | Contents | Notes |
|---|---|---|
| `$0000–$1FFF` | ROM | 8 KiB |
| `$2000–$23FF` | video RAM, the mirror where the CPU has priority | access disturbs the display on real hardware (§17) |
| `$2400–$27FF` | video RAM, the mirror where the CPU waits for the video circuit | the ROM uses this one; `$2700–$27FF` is undisplayed and used as workspace |
| `$2800–$2BFF` | character RAM, CPU-priority mirror | write-only |
| `$2C00–$2FFF` | character RAM, waiting mirror | write-only |
| `$3000–$3FFF` | 1 KiB user RAM, mirrored four times | system variables at `$3C00`; dictionary grows up from there |
| `$4000–$FFFF` | expansion RAM if fitted, otherwise unpopulated | a 16 KiB pack fills `$4000–$7FFF` |

Three facts here decide behaviour. M3 settled what the ROM needs of two of
them (§16), but not the hardware's values:

- **What a read of character RAM returns.** The CPU cannot read it back, but a
  read returns *something*, and a program that tests it would see that.
  Settle from the schematic. `$FF` meanwhile, as configuration.
- **What unpopulated memory reads.** The ROM sizes RAM at boot by writing
  `$FC` a page at a time from `$3D00` and reading it back (`$0028`), so any
  value but `$FC` puts RAMTOP in the right place. EL §4.1 says to model open
  bus rather than assume `$FF`. What the Ace's bus actually floats to is
  still to settle; `$FF` meanwhile, as configuration.
- **Which mirror the ROM writes the screen through.** The waiting one,
  `$2400` (and the character set through `$2C00`). This decides how much
  wait-state modelling matters (§6.4).

### 2.3 I/O

The Ace decodes I/O on **A0 alone**: any even port is the keyboard/speaker/tape
port. It is conventionally written `$FE`. The documented behaviour:

- **`IN` from an even port**: reads the keyboard half-rows selected by **low
  bits of A8–A15**, with keys on D0–D4, active low. It reads the tape input bit
  as well. As a side effect it **drives the speaker/tape output one way**.
- **`OUT` to an even port**: drives the speaker/tape output the other way. The
  data byte is ignored (to be confirmed).
- **Odd ports**: nothing on the stock machine; add-ons live here (§17).

The `IN`/`OUT` pairing is how the ROM's `BEEP` and tape writer make a
waveform. The polarity does not matter to audio, because the DC blocker
removes it (§8), but the recorder needs the edges (§10.4).

### 2.4 Expected keyboard matrix

Listed so that the code has a starting table. It is **settled by executing the
ROM** (§9.3), not by this table.

| Address high byte | D0 | D1 | D2 | D3 | D4 |
|---|---|---|---|---|---|
| `$FE` (A8 low) | SHIFT | SYMBOL SHIFT | Z | X | C |
| `$FD` | A | S | D | F | G |
| `$FB` | Q | W | E | R | T |
| `$F7` | 1 | 2 | 3 | 4 | 5 |
| `$EF` | 0 | 9 | 8 | 7 | 6 |
| `$DF` | P | O | I | U | Y |
| `$BF` | ENTER | L | K | J | H |
| `$7F` | SPACE | M | N | B | V |

Editing functions are SHIFT with a digit. As we understand it: SHIFT+1 DELETE
LINE, +2 CAPS LOCK, +3 TRUE VIDEO, +4 INVERSE VIDEO, +5 cursor left, +6 down,
+7 up, +8 right, +9 GRAPHICS, +0 DELETE. SHIFT+SPACE is BREAK. Punctuation is
SYMBOL SHIFT with a letter or digit.

### 2.5 Video

32×24 cells of 8×8 pixels, 256×192, one bit per pixel. Each screen byte's low
seven bits index the 128-character set in character RAM. Bit 7 inverts the
cell. The ROM writes the character set into character RAM at reset, so the
machine needs **no separate character ROM**. The image is a pure function of
768 screen bytes and 1,024 character bytes (EL §5.1). There are no modes, no
attributes and no border colour.

### 2.6 What the Ace does not have

No timer, no ULA, no sound chip, no video modes, no NMI source on the stock
board, no disc. Every one of those is a device the previous emulator had to
tick, decode or keep in sync, and the Ace does not need them. The run loop
checks two things per instruction: the slice end and the INT line (§5.3).

---

## 3. Host target and budgets

### 3.1 Boards and clocks

**RP2350 only**: Pico 2, Pico 2 W and Pimoroni Pico Plus 2 W (HW §2.1). One
`pico2` image runs on all three, and logs the physical board separately from
the build target (HW §2.1, §8.1). The RP2040 is dropped for CPU, not memory,
reasons (§17).

**Host clock 150 MHz**, the RP2350's rated clock and one that keeps the LCD's
SPI at 75 MHz (HW §3). The 300 MHz overclock is deferred (§18 item 3).
Whether 150 MHz is enough is the project's first measured question (§3.2).

### 3.2 CPU budget, and the gate on it

The risk is the Z80 interpreter on core 0. The arithmetic, written first so
the measurement can replace it (EL §14.4):

| Quantity | Estimate | Basis |
|---|---|---|
| Guest T-states per second | 3,250,000 | §2.1 |
| Mean T-states per guest instruction | 7–9 | Forth's inner interpreter is short register and memory ops |
| Guest instructions per second | 360–460 k | the two lines above |
| Host cycles per guest instruction | 180–280 | the 6502 measured 158–218 (EL §1); Z80 decode has prefixes, 16-bit ops, more flags |
| Core 0 share at 150 MHz | **43–86 %** | |

**Calibration from pico-atom** (its design §6.3, Plus 2 W, 2026-09-23, in
the mode that ships). At 1 MHz the Atom ran 2.5–3.5 guest cycles per
instruction, so ~290–400 k instructions/s. That took 35–46 % of core 0 at
158–218 host cycles each, with idle at the prompt the heaviest workload. At
2 MHz, about twice the instruction rate, it took **86 % of core 0 idle with
zero underruns over all four workloads**, and that configuration shipped.

So the Ace runs about 1.0–1.6× the 1 MHz Atom's instruction rate. If a Z80
instruction costs what a 6502 one did, the share is 35–75 %. At 1.3× the
cost, which allows for prefixes, 16-bit operations and more flags, it is
45–95 %. EL §1 warns that interpreter estimates come in 3–4× low. This one is
scaled from a measured result, not guessed, and its range is honest about the
Z80's unknown cost per instruction.

Two things work in the Ace's favour. The Atom's ~100 Thumb instructions per
guest instruction included a VIA tick and an IRQ line on every instruction,
and the Ace has no devices to tick (§2.6). A `HALT` fast-forward would also
help if the ROM waits for keys by halting (§5.3). It does not: M3 found it
spins at the prompt (§16).

**Measured, M2, 2026-10-03** (Plus 2 W, RP2350B rev 2, id
`7458DC82A89AAC12`; 150 MHz; gcc 15.2 `-O3`; `pico-ace-bench` at
`7ba7b7d` plus M2's changes). The Z80 alone on a flat bus, ten guest
seconds per run, eight runs per image, which agreed to within 0.01 %. Logs:
`out/m2-bench-t0.log`, `out/m2-bench-t2.log`.

| Workload | T per insn | Host cycles per insn, flash (tier 0) | SRAM (tier 2) | Core 0 at 3.25 MHz, tier 2 |
|---|---:|---:|---:|---:|
| Forth-shaped loop (§15.2 M2) | 7.82 | 81.8 | 82.8 | 22.9 % |
| ZEXDOC, first group | 8.09 | 103.4 | 86.0 | 23.1 % |
| *Estimate above* | *7–9* | *180–280* | | *43–86 %* |

The mean T per instruction landed inside the estimate. The cost per
instruction came in at **a third to a half of it**, below even the 6502's
measured 158–218: EL §1's warning that estimates come in low did not apply
here, and this estimate was scaled from a different interpreter. Tier 2
(the interpreter in SRAM, 30.5 KiB) was worth 1.20× on ZEXDOC's wide
instruction mix and nothing on the Forth loop, whose opcodes already fit
the XIP cache (−1.2 %, a relayout). That is the shape HW §9.8 describes.
Tier 1 is empty until the Ace's bus exists (`hot.h`), so it was not a
separate image.

**What the bench leaves out**, so the real share will be higher: the Ace's
bus (character RAM's slow-path writes, port I/O on every keyboard read),
the field interrupt and its split slices (§11.1), the real ROM's
instruction mix (M3), and **core 1 sharing the 16 KiB XIP cache** with the
LCD and menu code. The last argues for shipping tier 2, which takes the
interpreter out of that cache; M12 decides.

**The gate decision, 2026-10-03: 150 MHz is enough.** The projection is
23–28 % of core 0 against the ~85 % threshold, a margin of three times
before any lever. None of the levers below is needed for M7. CPU state in
locals (§5.2) moves to M12 as an optional experiment, and 300 MHz stays
deferred (§18 item 3). M7's measurement of the whole machine is the next
check on this.

**The gate.** Milestone M2 (§15) puts the Z80 core alone on the board and
measures host cycles per instruction on ZEXDOC and on a Forth-shaped loop,
before any other port work. M7 then measures the real share. The Atom's 2 MHz
guest shows ~85 % of core 0 is workable with zero underruns, so that is the
threshold, not the margin we would like. Above it, the levers in order are:
SRAM placement in measured tiers (EL §3.4, HW §9.8, worth 1.12–1.19× on the
6502), `HALT` fast-forward, flag evaluation deferred to where flags are read,
and only then **taking the deferred 300 MHz option back up** (§18 item 3).
pico-atom's `board_init_clocks()` already does the 300 MHz sequence, and its
soak passed there.

### 3.3 SRAM budget

All estimates, against 520 KiB. For scale, pico-atom measured 49 % used
with 64 KiB of guest memory, a 64 KiB tape buffer and 25 KiB of interpreter
in SRAM. Keep every fixed capacity in `src/core/config.h` and print `arm-none-eabi-size` on every build (EL §2.1).

| Item | Bytes (est.) | Notes |
|---|---:|---|
| Guest memory: ROM 8 K + video 1 K + char 1 K + user 1 K + expansion up to 48 K | 60 K | statically sized for the largest RAM option |
| Tape image buffer | 64 K | decompressed `.tap`, and the recorder's output (§10.3) |
| Frame snapshots, 3 × (768 + 1,024 + status) | 5.5 K | §4.4 |
| Presenter shadow + two RGB565 line buffers | 2.5 K | §7.3 |
| Audio ring (aligned) + PCM queue | 6 K | HW §5.3, EL §6.2 |
| Interpreter and hot paths moved to SRAM | 20–40 K | a measured tier, not a promise (§3.2) |
| FatFs, sector buffers, settings text | 8 K | |
| Stacks, both cores | 8 K | measure high water |
| Log ring | 4 K | EL §2.3 |
| **Total** | **~180–200 K** | **35–38 %** |

SRAM is not this project's constraint, unlike the previous one's (EL §1). Spend
the slack on SRAM code placement if §3.2 needs it, not on features.

---

## 4. Architecture

### 4.1 Core and port

As EL §2.1 describes: a portable **core** (Z80, bus, Ace machine, keymap data,
media formats, snapshot, settings parser, frame pool, row generator) with no
SDK header and no allocation, and a **port** (LCD, audio, southbridge, SD,
menus, the two cores' loops). Both build under `-Wall -Wextra -Werror`, the
core under CTest on the workstation and everything under `arm-none-eabi-gcc`,
in CI on every push.

The layout, build options and names follow pico-atom so code moves between
the two with a rename (§4.6). `PICO_ATOM_` becomes `PICO_ACE_`, and `atom_`
becomes `ace_`.

```
src/core/    config.h  hot.h  z80.c  ace.c  render.c  font.c  snappool.c  beeper.c
             keymatrix.c  keymap_picocalc.c  keylayout.c  tape.c  cassette.c
             tap.c  snap_ace.c  snapshot.c  settings.c  status.c
src/port/    main.c  core0.c  core1.c  menu_*.c  board.c  lcd.c  display.c
             southbridge.c  kbd.c  audio.c  log.c  sd.c  diskio.c  storage.c
             roms.c  tapeio.c  snapio.c  settingsio.c  keymapio.c  textpage.c
test/host/   one binary per area; test_util.h (CHECK, TEST_DONE); guest.c; golden/
tools/       build.sh  flash.sh  uart-log.sh  uart-type.sh  perf-run.sh
             perf-summary.sh  soak.sh  soak-check.py  fetch-test-suites.sh  trace/
cmake/       pico_sdk_import.cmake  version.cmake
```

The build options are `-DPICO_ACE_HOST=ON` for the host build,
`-DPICO_ACE_UART=OFF` for the build that ships, `PICO_ACE_RAM_TIER`, and
`PICO_ACE_BOOT_*`.

**Split `main.c` and the menu from the start.** pico-atom's `main.c` (core 1's
loop, parking, boot media, UART keys, measurement) and `menu.c` each grew to
about 1,200 lines, the two largest files in the tree. Here core 0's loop,
core 1's loop and each menu page get their own file.

### 4.2 The core API

EL §2.2's seam, with the Ace's two video inputs in place of a VRAM pointer and
a mode byte:

```c
void     ace_init(ace_t *m, const ace_config_t *cfg);   /* RAM size, ROM image */
void     ace_reset(ace_t *m);
uint32_t ace_run(ace_t *m, uint32_t t_states);          /* whole instructions; returns T actually run */
uint32_t ace_run_field(ace_t *m);                       /* one field, split at its edges (§11.1) */
void     ace_key_set(ace_t *m, int row, int col, bool down);
size_t   ace_audio_drain(ace_t *m, int16_t *dst, size_t max);
const uint8_t *ace_screen(const ace_t *m);              /* 768 bytes */
const uint8_t *ace_charset(const ace_t *m);             /* 1,024 bytes */
void     ace_copy(ace_t *dst, const ace_t *src);        /* never '=': the page table points into the struct */
```

`ace_run` follows the debt-carry rule: it runs whole instructions until at
least `t_states` have passed and returns the true count. There is no "exactly
N" variant.

### 4.3 Which core owns what

EL §2.3 unchanged. **Core 0**: the Z80, the machine, speaker synthesis, the
audio DMA IRQ, pacing. **Core 1**: LCD presenting, southbridge I²C (keyboard,
battery, backlight), SD card, menus, moving log bytes to the UART. Core 1
never reads guest RAM while the guest runs, and never calls `sleep_us`. Core 0
never calls `printf` once the guest runs.

### 4.4 Frame handoff

The three-buffer, four-state pool of EL §2.4, under one SIO spinlock, with
the drop counter on the heartbeat. A snapshot is:

| Field | Bytes |
|---|---:|
| screen (`$2400–$26FF`) | 768 |
| character set (`$2C00–$2FFF`) | 1,024 |
| status: field number, tape position, flags for the status line | ~16 |

At under 1.9 KiB, the copy at field end costs core 0 a few microseconds.

### 4.5 Parking

All card work, the menu, pause and restart go through EL §2.5's park: core 0
stops at a field boundary, feeds the audio queue silence at the drain rate,
and hands the machine to core 1. Guest time does not pass while parked.

### 4.6 What comes from pico-atom

pico-atom (`../pico-atom`, GPL-3.0, same author) is the emulator the lessons
came from. Its host layer was verified on the device, and much of its core
is not specific to the Atom. The licence that allows reuse is settled (§18 item 5). With
it, this is the plan:

**Copy, rename, re-verify.** These were checked on a Plus 2 W and depend on no
Atom part:

| pico-atom file | What it gives |
|---|---|
| `port/board.*` | 150/300 MHz with the rail and flash QMI timing moved first; physical board identity; die temperature by package |
| `port/lcd.*` | panel init (ClockworkPi's values, with the THIRD-PARTY entry), windows, fills, polled-DMA ping-pong blit |
| `port/southbridge.*` | register layer with timeouts and a busy flag; refuses to read `RST` |
| `port/kbd.*` | core 1 drains the FIFO into an SPSC ring for core 0 |
| `port/audio.*` | PWM slice, chained DMA, PCM queue, pacing, both counters, exact rate as a fraction |
| `port/log.*` | core 0's log ring drained by core 1 |
| `port/sd.*`, `diskio.c`, `storage.*`, `fatfs/ffconf.h` | SD over `spi0` and FatFs, mount per job; FatFs copied from the SDK at configure time |
| `port/settingsio.*` | the settings file written through `.new` and rename |
| `core/snappool.*` | the three-buffer handoff, lock-free and tested on the host. Only the snapshot struct changes (§4.4) |
| `core/beeper.*` | the box filter at a rational period and the DC blocker. Only the cycle rate changes (§8) |
| `core/sha1.*` | not needed in the firmware (the ROM is checked at build time, §10.2); kept only if a host tool wants it |
| `core/hot.h` | SRAM tiers by section name, with no SDK include |
| `core/settings.*` | parser and in-place rewriter with every rule of EL §8.7. Only the keys change |
| `core/status.*` | perf line and `PAUSED` formatting. The deck fields change |
| `tools/*`, `cmake/version.cmake`, `.github/workflows/ci.yml` | build, flash with `reset halt`, UART capture that refuses a second reader, paced typing, perf runs, soak and its checker; version from `git describe` |

**Adapt.** The structure carries over and the guest-specific parts change:

| pico-atom file | Change |
|---|---|
| `core/keymatrix.*` | the held set, canonicalisation, binding fixed at press, a reserved release slot for every open press, and paced replay all stay. The matrix becomes 8×5, and the lines become SHIFT and SYMBOL SHIFT |
| `core/keymap_picocalc.c`, `core/keylayout.c` | the table format, the Alt layer, `KM_MENU` rows as menu pages, and the `.map` parser stay. The cells come from §9.3's sweep |
| `port/display.*` | the dirty-band presenter, status and perf lines drawn only when their text changes, and the test pattern stay. Mode change becomes glyph change (§7.3) |
| `port/textpage.*` | a text page through the guest renderer. 32×24 cells with the emulator's own font (§7.5), not the MC6847's 32×16 |
| `core/tape.*`, `core/cassette.*` | the stall at a trapped handler, leaving the ROM's state, deck cues and the recorder's shape. The Ace's routines and `.tap` replace OSLOAD and UEF |
| `core/snapshot.*` | header, CRC, ROM hash, version, two passes. The fields are the Z80's and the Ace's |
| `test/host/guest.*`, `test_util.h` | the harness that boots the real ROM (here always the committed one) and types through the southbridge event path |
| `port/main.c`, `port/menu.c` | read for the park/hand-off, pause, boot media and machine restart, then split (§4.1) |

**Not used:** `m6502`, `i8255`, `mc6847` and its font, `via6522`, `i8271`,
`uef`, `inflate` (the Ace archive's `.tap` files are not gzipped),
`discio`, `portb`, and `romset` and `port/roms` (the ROM is embedded, not read from the card, §10.2).

Copied code keeps its tests. A module counts as reused only once its pico-atom
tests pass here under the new names.

---

## 5. The Z80

### 5.1 Accuracy

- Every opcode, **including the undocumented ones**: IXH/IXL/IYH/IYL forms,
  `SLL`, the `DDCB`/`FDCB` register-copy forms, flags bits 3 and 5 (X and Y),
  `MEMPTR` (WZ) and its leak into `BIT n,(HL)`, `R` incremented per M1 cycle,
  and the `LD A,I`/`LD A,R` P/V quirk. This differs from EL §3.1's "trap and
  count": Z80 undocumented behaviour is well documented, the test suites check
  it, and Spectrum-era code habits mean Ace software may use it.
- **Count `ED` holes** (the opcodes that behave as two-instruction NOPs) on the
  heartbeat. A nonzero count over a soak names a program worth looking at.
- `EI` delays interrupt acceptance by one instruction. `HALT` repeats NOP
  timing until an interrupt. IM 0, 1 and 2 are all implemented. The Ace uses
  IM 1, and IM 2 reads whatever floats on the bus during acknowledge (§16).
- No per-T-state bus accuracy (EL §3.1). Wait states are applied per access
  where §6.4 says so, at instruction granularity.
- **Q**, the flags register's shadow: `SCF` and `CCF` take X and Y from
  `(Q ^ F) | A`, where Q is F if the previous instruction wrote the flags
  and 0 if not (Patrik Rak's measurements of NMOS Z80s). FUSE cannot see
  this, since every FUSE test starts with Q at zero, so our own test checks
  it (§5.4).

**Where the sources disagree, FUSE's tests decide**, because they are what
M1 is checked against. Four choices follow, settled 2026-10-03 against
FUSE's `tests.expected` from its repository's master branch:

- **Repeating block instructions keep the single instruction's flags.**
  `LDIR`, `CPIR`, `INIR`, `OTIR` and their `D` forms have been measured since
  to change H, P/V, X and Y on each repeat. FUSE does not model that, and its
  `edb2_1`, which stops `INIR` part-way, expects `INI`'s flags. No known Ace
  program depends on the difference.
- **A `JR cc` or `DJNZ` not taken does not read its displacement.** A real Z80
  does read it, but a read with no side effects cannot be seen on the Ace's
  bus, and FUSE's access order omits it.
- **`HALT` leaves PC on itself** and runs again as a NOP until an interrupt
  steps past it. A real Z80 advances PC and ignores what it fetches. The two
  push the same return address and differ only in the PC a snapshot shows.
- **IM 0 executes only an `RST`**, from the bus byte. On the Ace that byte is
  believed to be `$FF` (§16), `RST $38`, the same as IM 1.

### 5.2 Implementation

A `switch`-dispatched interpreter with explicit T-state accounting, one
`switch` per prefix page, CPU state held in locals across the dispatch loop
(EL §3.2). `DD`/`FD` share one body parametrised by the index register. The
CPU calls the bus through inline fast paths for memory (§6.1) and an out-of-line
call for I/O. It knows nothing about the Ace.

Labels-as-values (computed `goto`) dispatch is a GCC option worth one
measured experiment in the perf pass, against a control build (EL §12). It is
not the starting point.

**As built in M1** (`src/core/z80.c`), the CPU state stays in `z80_t` and is
reached through its pointer, not copied into locals. Locals were to be
M2's first experiment, with M1's code as the control. M2's gate passed with
a margin of three times without them (§3.2), so the experiment moves to
M12, where it is optional. The memory fast path is the
page table of §6.1 (`page_t`, defined in `z80.h`), with the bus's functions
behind a NULL page, so the host tests use the same path. A test that logs
every access leaves every page NULL.

### 5.3 The run loop

```
while (t < slice_end):
    if int_line && iff1 && !int_blocked: accept interrupt (IM 1: 13 T)
    if halted: fast-forward to min(slice_end, next INT edge); R += skipped/4; continue
    execute one instruction
```

**`HALT` fast-forward** jumps time to the next event instead of interpreting
NOPs, and keeps `R` exact. If the ROM halts while it waits for a key, idle at
the prompt becomes nearly free. On the 6502, idle at the prompt was the
heaviest workload (EL §12). **M3 found that it does not** (§16): the prompt
spins on a flag at `$059B` that the interrupt sets, so fast-forward cannot
help there. The ROM does halt once a word in `VLIST` (`$0679`), and a program
may, so M12 measures it on those.

**The INT line is a level with a duration**, not a pulse at a point. A Z80
samples INT at the end of each instruction. If the Ace holds INT for a fixed
time and the guest has interrupts disabled for longer than that, the interrupt
is lost. A program that does that loses frames on a real Ace as well. So the
INT length is a constant to settle (§16), and the field split stops a slice at
both edges (§11.1).

### 5.4 Test suites

| Suite | What it proves | Notes |
|---|---|---|
| **ZEXDOC** and **ZEXALL** (Cringle) | documented and undocumented flag results over every instruction group | run under a minimal CP/M BDOS stub (`CALL 5`, functions 2 and 9). A run takes minutes on the host; register it as a long test |
| **FUSE's Z80 tests** (`tests.in`/`tests.expected`) | per-opcode T-states, memory and port access sequences, `MEMPTR` | **the cycle table asserted by execution** (EL §3.3) |
| Behaviour tests (ours, `test_z80_behaviour`) | `EI` delay, `HALT` and `R`, IM 0/1/2, INT held vs. pulsed, the `LD A,I` P/V quirk, NMI and `RETN`, `ED` holes, the run contract, prefix chains, Q | written against the Z80 user manual and Rak's measurements; each rule beside a control that must fail |

Fetch the binaries with `tools/fetch-test-suites.sh` into a gitignored
directory, and commit none (both suites are GPL). A missing binary makes the
test exit 77, and **a skipped ZEXALL is an unverified CPU** (EL §3.3). CI
fetches them on every run.

`test_z80_fuse` checks every register, `MEMPTR`, I, R, the IFFs, IM, the
halt state, the T-states, all 64 KiB of memory, and the order of memory and
port accesses with their addresses and data. It does not check when within
an instruction each access happens, or FUSE's contention events: the
emulator is exact per instruction, not per T-state (§5.1).

**The harness was shown to fail**, 2026-10-03, with one bug planted at a
time in a copy of the tree: `DJNZ` taken at 12 T, `LD A,(nn)` leaving
`MEMPTR` at `nn`, `EX (SP),HL` writing its two bytes in the other order, and
`BIT n,(HL)` taking X and Y from the operand each fail FUSE. `SCF` without Q
passes FUSE, as expected, and fails `test_z80_behaviour`.

---

## 6. Bus and memory

### 6.1 Page table

256 pages of 256 bytes, each a `{read, write}` pointer pair, NULL for the slow
path, with flags in a separate byte array (EL §4.1). On the Ace the table
does the mirroring for free:

| Pages | `read` | `write` |
|---|---|---|
| `$00–$1F` | ROM | NULL (ROM writes are ignored, in the slow path) |
| `$20–$27` | video RAM, `(page & 3) * 256` | same |
| `$28–$2F` | NULL (slow path returns §2.2's value) | character RAM, `(page & 3) * 256` |
| `$30–$3F` | user RAM, `(page & 3) * 256` | same |
| `$40–…` | expansion RAM up to the configured size | same |
| above | NULL (open bus) | NULL |

Only the character RAM read and unpopulated accesses take the slow path. Every
ROM fetch and every RAM access is one indexed load.

### 6.2 Configurations

| Name | User RAM | Range | Use |
|---|---|---|---|
| **Ace 3K** (stock) | 1 KiB | `$3C00` + mirrors | the machine as sold |
| **Ace 19K** (**default**) | 1 + 16 KiB | `+ $4000–$7FFF` | the common pack; most software expects it |
| **Ace 51K** | 1 + 48 KiB | `+ $4000–$FFFF` | the largest; for big programs |

With a pack fitted, are the mirrors of the 1 KiB still present at
`$3000–$3BFF`? We believe so, since the pack decodes from `$4000` (§16). The
power-on default is **19K** (§18 item 1). The RAM size changes only through
a restart that behaves as a power-on (EL §10).

### 6.3 Power-on state

Zero-filled RAM (EL §9.2), with one thing to look for: **a random seed the ROM
keeps in RAM**. If Ace Forth's random-number word reads a seed that zero RAM
leaves stuck, seed it from the RP2350's TRNG in firmware and from a constant
in host tests. **Settled in M3: there is none.** The ROM has no random-number
word; the manual's `RND` is user code that seeds from FRAMES (`$3C2B`), which
counts fields from power-on (§16).

### 6.4 Video memory wait states

On a real Ace, a CPU access to the `$2400`/`$2C00` mirrors during active
display waits until the video circuit is not fetching. The ROM writes the
screen through the waiting mirror, so printing on a real Ace is slower than on
an emulator without wait states. A game timed by its own drawing loop would
run fast.

**The first version models no wait states.** M14 measures how much they matter:
trace-diff the ROM printing a screenful against the reference emulator
(§13.4), and time a known game's frame loop. If it matters, model wait states
per access from the T-state within the line, in the slow path. That costs the
waiting mirror's fast path, so measure it against a control build (EL §12).

### 6.5 I/O

The Z80's `IN`/`OUT` go to one function, `ace_io_read`/`ace_io_write`, which
tests A0. Even port reads build D0–D4 from the matrix rows selected by A8–A15
(several rows ANDed), add the tape bit, set the remaining bits to the
settled value (§16), and drive the speaker (§8). Odd ports return open bus.
Decode **by mask**, never by equality with `$FE` (EL §4.1).

---

## 7. Video

### 7.1 No framebuffer

The image is a function of the snapshot (§2.5), so there is no framebuffer
(EL §5.1). Core 1 generates RGB565 rows from the snapshot straight into two
DMA line buffers (HW §4.6).

### 7.2 The row generator

Per pixel row *y* of a character row: for each of 32 cells,
`bits = charset[(code & 0x7F) * 8 + (y & 7)] ^ (code & 0x80 ? 0xFF : 0)`,
then expand 8 bits to 8 RGB565 pixels through a **256-entry × 16-byte LUT**
(4 KiB, built once, rebuilt only if the colour pair changes). Copy each 16-byte
run unrolled, not with `memcpy` (HW §9.4). The generator is portable C in the
core, so the host's golden-image tests run the firmware's code.

### 7.3 Dirty bands

24 bands, one per character row, each with an inclusive `[min..max]` cell
span (EL §5.3). Per presented snapshot:

1. **Diff the character set against the shadow** and build a 128-bit
   "changed glyph" mask. This is the Ace's version of EL §5.3's mode byte: a
   program that redefines a character changes every cell showing it while
   leaving the screen bytes identical. A screen-only diff would miss it.
2. Mark a cell dirty if its screen byte changed **or** its glyph's bit is set
   in the mask.
3. Send each dirty band's span as one LCD window, 8 rows tall.
4. Copy the snapshot into the shadow.

The diff is per cell and exact, so over-marking is limited to the columns
inside a band's span. Character-set animation is a common Ace technique, and
it repaints exactly the cells using the changed glyphs.

### 7.4 Geometry

The guest is drawn **1:1 at (32, 64)** on the 320×320 panel (EL §5.4, HW
§4.10). The 64-row band above it holds the **status line** (tape, RAM size,
warnings) and the band below it the **perf line**. Each is drawn only when its
text changes. The border is black and never redrawn. A full guest redraw is
~11.5 ms of wire at 75 MHz (EL §1), well inside a 20 ms field.

Tearing is accepted (EL §5.4). Small band presents confine a tear to one
character row.

### 7.5 The character set

The guest's characters come **from guest character RAM, always**, as the ROM
wrote them. Nothing substitutes blanks at power-on: before the ROM writes the
set, the screen shows whatever zeroed character RAM gives, and a test pins
that (EL §5.5).

The emulator's own pages (menu, About) need a font that does not depend on
what a program has done to character RAM. They use a **public-domain 8×8 font** kept in the tree
with its licence and attribution. It is rendered to an image and checked by
eye once (EL §5.5). As built in M4: Daniel Hepper's `font8x8_basic.h`,
after IBM's public-domain VGA fonts, kept unmodified in `third_party/font8x8/`.
`tools/mkfont.py` writes it to `src/core/font.c` with each byte reversed so
that bit 7 is leftmost, and CI checks the two agree. `test_render` checks
its `A` against the upstream README's drawing. The menu fills its own 768-byte screen and supplies its
own 1,024-byte character set, and goes through the same row generator (§12).

### 7.6 Golden images

Fixed screens with the ROM's character set, inverse cells, a redefined
character and every glyph, rendered to PPM and committed after being looked at
(EL §5.7). As built in M4, `test_golden` renders five scenes and compares
them with `test/host/golden/`: `boot` (the real ROM after `2 2 + .`),
`inverse`, `redefined`, `glyphs` (all 256 codes in the ROM's set) and
`font` (the same in §7.5's font). The ROM's set is the one it writes at
boot, read back from character RAM. `test_golden --write DIR` writes them
for inspection; on a mismatch the test writes `<name>.actual.ppm`.

---

## 8. Audio

EL §6 applies whole. The Ace specifics:

- **The level changes on every even-port access**: `IN` drives it one way,
  `OUT` the other. An `IN` used only to read the keyboard therefore also moves
  the speaker. That is the Ace's real behaviour, and it is why keyboard
  scanning on an Ace clicks. If the ROM's key scan makes an audible tone at the
  prompt, that is authentic. Find out on the host before deciding anything.
- **Box filter** over guest T-states, stamped at the start of the accessing
  instruction (EL §6.1). The sample period is a reduced rational: at 150 MHz,
  TOP 2047 and oversample 2, the host rate is 150,000,000 / 4,096 samples/s,
  so **one sample is 6,656/75 T** (≈ 88.75). A field of 64,896 T is exactly
  731.25 samples. Advance by quotient and remainder, with no division per
  sample.
- One-pole **DC blocker**, silence is 0, fixed point throughout.
- **Pace on the audio queue** (EL §6.3). Audio and the guest share `clk_sys`.
- Test: the ROM's `BEEP` at a few pitches, measured against the cycle count of
  its loop, and every sample against an independent edge model to 1 LSB.

---

## 9. Keyboard

### 9.1 The path

EL §7.1's backwards mapping: PicoCalc `[state, code]` events → normalised →
held-key set → binding fixed at press → (row, col, guest SHIFT, guest SYMBOL
SHIFT) → the matrix, **replayed at the guest's pace** (each key held a minimum
number of fields with a gap after). The minimum hold is read off the ROM's
key routine in M5.

### 9.2 The standard map

The map is data, one row per PicoCalc code (EL §7.2):

| PicoCalc | Ace | Note |
|---|---|---|
| `a`–`z`, `0`–`9` | the key | |
| `A`–`Z` | SHIFT + key | the PicoCalc's own Caps Lock works through this |
| punctuation | SYMBOL SHIFT + the key the ROM sweep finds (§9.3) | the shifted-only characters (`\|` `{` `}` `` ` `` `~`) need entries of their own (EL §7.2) |
| Enter, Space | ENTER, SPACE | |
| Backspace | DELETE (SHIFT+0) | |
| ← ↓ ↑ → | SHIFT+5 / 6 / 7 / 8 | guest SHIFT is ours, so the swallowed host Shift+arrow chords cost nothing (HW §6.3) |
| Esc | BREAK (SHIFT+SPACE) | the host's Shift+Space never arrives (HW §6.3), so BREAK needs a plain key |
| **Shift** held | asserts guest SHIFT, **except while a key it shifted on the PicoCalc is down that the Ace types without SHIFT** | games read SHIFT alone (EL §7.2). The exception is pico-atom's `unshift` flag: on the PicoCalc `:` `"` `!` and the like are Shift chords, but on the Ace they are SYMBOL SHIFT chords. If the host's Shift reached the matrix as well, the Ace would see SHIFT+SYMBOL SHIFT and type something else |
| **Ctrl** held | asserts guest SYMBOL SHIFT | Ctrl+key reaches every symbol-shifted cell raw, which the MCU delivers unchanged (HW §6.3) |

**Alt layer** (the Ace has no Alt, so nothing is stolen from it; a key with
Alt down comes from this layer only):

| Chord | Action |
|---|---|
| Alt+L | CAPS LOCK (SHIFT+2) |
| Alt+G | GRAPHICS (SHIFT+9) |
| Alt+V | INVERSE VIDEO (SHIFT+4) |
| Alt+T | TRUE VIDEO (SHIFT+3) |
| Alt+X | DELETE LINE (SHIFT+1) |
| Alt+M | menu |
| Alt+P | pause |
| Alt+R | reset (asks first) |

Avoid Alt+`,` `.` Space `B` (MCU's own) and Alt+I (the MCU's Insert) (HW
§6.3). F1–F5 and F10 open emulator pages directly (§12).

### 9.3 Settling the matrix by execution

In M5, on the host: boot the ROM, then press each of the 40 cells at the `OK`
prompt alone, with SHIFT and with SYMBOL SHIFT, and read what the ROM puts in
screen RAM. That gives the whole map, including the editing keys and every
punctuation character's cell. Keep the sweep as a regression test that types
every entry of the PicoCalc table through the real ROM (EL §7.2). A host test
also checks that every code maps to exactly one binding and that no binding
needs a chord the MCU swallows.

### 9.4 Game layouts

Overlays on the standard map, generic in firmware and per-game in `.map`
files on the card, selected by the tape or snapshot they name (EL §7.3). The
file syntax is pico-atom's (`name =`, `tapes =`, one `<PicoCalc key> = <Ace
target>` a line), so its parser comes across with only the target names
changed (§4.6). Ace games
commonly use 5–8, Q/A/O/P and Z/X, so the built-ins are "cursor keys as
5–8" and "arrows as Q/A/O/P", with fire on a key the arrows can chord with.

---

## 10. Media and settings

### 10.1 The card

```
/ace/
  pico-ace.cfg          settings (§10.6)
  tapes/*.tap           tape images; the recorder writes here too
  snaps/*.ace           snapshots from the archive (§10.5)
  states/*.sav          our own save states (§10.5)
  keymaps/*.map         game layouts (§9.4)
```

The same shape as pico-atom's `/atom/`, so the two can share a card. The
card is optional: the ROM is in the firmware (§10.2), and without a card the
machine runs on the defaults, with no tapes or saved states.

### 10.2 The ROM

**The project ships the ROM** (§18 item 2). It lives at `roms/ace.rom` in
the repository, with the permission it is distributed under in
`roms/COPYING.md` and an entry in `THIRD-PARTY.md`.

| | |
|---|---|
| Size | 8,192 bytes |
| SHA-1 | `597ba8a15a292688333c84dc9fd35172abe5e7e6` |
| Halves | `$0000–$0FFF` CRC32 `dc8438a5`, SHA-1 `8fa97eb71e5dd17c7d190c6587ee3840f839347c`; `$1000–$1FFF` CRC32 `4009f636`, SHA-1 `98c5d4bcd74bcf014268cf4c00b2007ea5cc21f3` |

The halves match the two 4 KiB dumps that MAME's `jupace` driver lists (to
be confirmed against its source, §16). The contents are the Ace's: Forth
word names such as `CONTEXT` and `CURRENT`, and a keyboard table at the
point the ROM scans the keys.

**The firmware embeds it.** CMake converts `roms/ace.rom` into a `const`
array at build time and **fails the build if its SHA-1 differs** from the
one above, so a damaged or substituted file cannot boot and then misbehave
(EL §8.1). The ROM runs from flash through the XIP cache like the code, or
from SRAM if M12 measures that it pays. There is no ROM on the card, so
there is no missing-ROM page: the machine boots to `OK` with no card fitted.

The host tests and CI use the same file, so the tests that run the real ROM
(§13.3) always run, and never skip.

### 10.3 Tape, phase 1: trapping the ROM

The Ace's tape words (`SAVE`, `LOAD`, `VERIFY`, `BSAVE`, `BLOAD`, `BVERIFY`)
end in ROM routines that write and read one block (a header or the data).
Trap the **block handler**, check its first bytes against the stock ROM, and
stand aside for anything else (EL §8.2):

- **Stall the CPU at the trap** as if `WAIT` were held, so the port serves the
  block at the next field boundary with the guest parked (§4.5). A request no
  file answers is declined, and the ROM's own routine runs.
- **Leave what the ROM leaves**, byte for byte: registers, flags, the header
  buffer, system variables, the speaker level. Test it by running the ROM's
  own routine with only its bit-level I/O hooked and requiring the trapped
  call to leave the same machine in every byte of RAM.
- **Format: `.tap` as the Ace community uses it.** As we understand it, each
  block has a 2-byte little-endian length, then the block bytes, then a
  checksum. A header is the file type (dictionary or bytes), a ten-character
  name, the length, the start address and several dictionary pointers. All of
  this is unverified (§16). Settle it against the ROM's own routines and
  against files from the archive, not against a description.
- `SAVE` appends a header and data block to the selected tape image through
  `.new` and rename (EL §8.6).

### 10.4 Tape, phase 2: the signal

The ROM reads the tape bit with its own timing loops. Decode `.tap` blocks to
**half-cycles clocked in T-states**, presented on the input bit and brought up
to date only when the port is read, with a next-edge countdown (EL §4.3,
§8.3). This loads protected or custom loaders and makes turbo free. Read the
ROM's routines first for leader, sync and bit timing, and carry remainders so
half-cycles never drift. Recording decodes the output bit back into blocks.
`.wav` is out of scope (§17).

### 10.5 Snapshots

**`.ace` (import only; export is deferred, §18 item 4).** This is the archive's common format,
written by xAce and EightyOne: as we understand it, a run-length-encoded image
of memory from `$2000` with the Z80 registers stored in the undisplayed
workspace. Settle the encoding, the register layout and the RAM size it
implies from those emulators' documentation and from sample files (§16). It
is **refused** if the implied RAM size differs from the running machine's,
naming the size it needs.

**`.sav` (our save states).** EL §8.5 exactly: explicit little-endian fields,
magic, version, lengths, CRC, every field zero at reset, ROM hash not ROM
bytes, RAM size and guest clock recorded and checked, and a two-pass load.

### 10.6 Settings

`/ace/pico-ace.cfg`, `key = value`, edited in place, parsed back before writing,
saved only by a menu action, no flash writes (EL §8.7). Keys: `ram`
(`3k`/`19k`/`51k`, default `19k`), `volume`, `layout`,
`boot_tape`, `perf_line`. Build-time `BOOT_*` overrides win (EL §13.1).

---

## 11. Timing

### 11.1 The field

One field is 312 lines × 208 T = **64,896 T** at 50.08 Hz (§16, medium).
`ace_run_field` splits it at three points, and debt carry keeps the split
exact (EL §2.2, §5.6):

1. **INT asserted** at the start of vertical sync. A run stops here, so the
   interrupt is taken at the right instruction.
2. **INT released** after its settled duration (§5.3).
3. **The first active display line.** The snapshot is published here, so a
   program that redraws after the interrupt has finished before the snapshot
   (EL §5.6).

Until the line numbers are settled, they are runtime configuration. Test with
a guest loop that `HALT`s for INT and redraws: every snapshot must show a
finished redraw. Run a single-point field shape as a **control that must
fail**.

### 11.2 Turbo

Turbo runs unpaced while a tape plays, as in EL §9.3. There is no faster
guest-clock option (§17), so EL §9.3's table of clock-following quantities
does not apply.

### 11.3 The host clock

150 MHz, fixed (§18 item 3). `main()` still sets the core rail explicitly,
because the regulator survives a reset and a board last run by other
firmware at 300 MHz would otherwise keep 1.20 V (HW §3). If the 300 MHz
option is ever taken up, it follows EL §9.4's sequence through pico-atom's
`board_init_clocks()`.

---

## 12. User interface

EL §10, with the Ace's specifics:

- **Boot straight to `OK`.**
- **Alt+M** opens the menu, which pauses the guest. Pages: Tape (select,
  play/stop, new recording), Snapshot (load `.ace`, save/load `.sav`), Machine
  (RAM size, staged, applied by power-on restart, with a warning
  that the program is lost), Layout, Settings (save), About.
- **F1** Tape, **F2** Snapshot, **F3** Machine, **F4** Layout, **F5** About,
  **F10** menu. A page opened this way returns to the guest when closed.
- The menu is a 32×24 text page rendered through §7.2's generator with the
  emulator's own font. Closing it invalidates the shadow, so the next
  snapshot is presented whole.
- **Pause** dims the backlight (read it first, restore it after) and shows
  `PAUSED`. Any key resumes and is not passed on.
- **About**: firmware version from `git describe --always --dirty` at build
  time, physical board, chip revision, clocks, southbridge version, die
  temperature, the ROM's SHA-1, RAM size, settings file state.
- A **status row** in the menu names the first problem.
- Firmware names no titles. Per-title configuration lives on the card.

---

## 13. Testing

EL §11, applied:

### 13.1 Shape

No framework: `test/host/test_util.h` with `CHECK` and `TEST_DONE`, one binary per
area, CTest, exit 77 for a skip. Behaviour is asserted by execution, and
every test of a timing rule has a control that must fail.

### 13.2 Host tests by area

| Area | Tests |
|---|---|
| Z80 | ZEXDOC, ZEXALL, FUSE opcode tests, interrupt and `HALT` timing (§5.4) |
| Bus | every mirror reads and writes the same byte; character RAM write-only; ROM unwritable; unpopulated reads; even/odd port decode by mask |
| Video | golden images (§7.6); glyph-change dirty marking with a control that diffs the screen only and must miss the change; zeroed character RAM at power-on |
| Frame pool | randomised interleaving of both cores' transitions |
| Audio | edge model to 1 LSB; rational sample count after any run; DC blocker settles |
| Keyboard | every code maps to one binding; no swallowed chords; release undoes press across a layout change |
| Media | `.tap` parse and write round trip; `.ace` decode of sample files; `.sav` round trip; torn and foreign files leave the machine unchanged |
| Settings | in-place edit keeps comments, order and line endings; duplicate key refused; rewrite parses back |

### 13.3 The real ROM on the host

A harness boots the committed `roms/ace.rom`, checking its SHA-1 first
(EL §11.2). The ROM is always present, so these tests never skip, and CI
runs them on every push. Using the firmware's matrix path it:

- boots to `OK` and checks the screen;
- types `2 2 + .` and reads ` 4 OK` back (exact text settled on first run);
- sweeps the keyboard matrix (§9.3);
- defines a word, `SAVE`s it through the ROM's own signal path, decodes the
  output bit with an independent decoder, writes a `.tap`, `LOAD`s it back
  through the ROM, and runs the word (EL §11.3);
- does the same through the phase-1 trap and requires identical RAM;
- runs `BEEP` and measures its pitch;
- restores a `.sav` mid-program into a machine doing something else, runs 150
  fields and requires identical state.

### 13.4 Trace diff

Against **xAce** (plain C, built from its own checkout with stub X11
headers), printing `PC AF BC DE HL IX IY SP T` and the opcode bytes, with
resync after reads of the keyboard port and INT acceptance (EL §11.4). MAME's
`jupace` driver is a second opinion on hardware facts, read but not run.
Check each reference against the Z80 manual before believing it on cycles.
Plant one bug to prove the harness catches it. pico-atom's
`tools/trace-diff.py` (run, resync, classify, carry the reference's errata)
is the shape to copy. Only its trace format and resync points change.

**Get xAce building in M3, not at the end.** pico-atom built its reference
early for trace diffs, but left "a recording loads in another emulator" to
its last milestone. That check is still outstanding there, because the
other emulator "could not be got working". Here the reference is built in
M3, loads one archive `.tap` and one `.ace`, and stays in use, so M13's
recording check has a working reference waiting for it.

**As built in M3, 2026-10-03.** `tools/trace/build-xace.sh` clones
[xAce](https://github.com/lawrencewoodman/xAce) at `52d89b2` and compiles its
`z80.c` and `tape.c` where they stand, with one line added by `sed` (a trace
hook at the top of the CPU loop, where the registers are locals). Its X11
front end is not built at all: `tools/trace/xace-trace.c` replaces
`xmain.c`, so no X11 headers are needed. `ace-trace` (host build) is our
half; `tools/trace-diff.py run --keys …` runs both and diffs them. CI runs
it on every push.

There is **no resync**. xAce's interrupt is a wall-clock `SIGALRM` and its
timing has errata, so the driver makes the two keep the same time instead:
INT over the same window of each field (taken at most once, since xAce
clears no IFF on accepting it), 13 T for the acknowledge xAce does not
count, and its two timing errata corrected against the Z80 manual (`LD C,n`,
`LD E,n`, `LD L,n`, `LD A,n` at 4 T instead of 7; `RES`/`SET b,(HL)` at 12
instead of 15). The traces then agree line for line, and every difference
left is in a named class: registers' power-on values (`$FFFF` here, 0 in
xAce), F's bits 3 and 5 (xAce does not model them), and two flag errata
(`ADC HL,ss` sets N in xAce; its `BIT` never sets S). The first `HALT`
ends the comparison, because xAce's `HALT` is a NOP.

Results, `out/m3-trace-diff.log`: boot to the prompt and 10 fields, 59,690
instructions, no divergence; boot, typing `2 2 + .` and running it, 915,615
instructions, no divergence; `VLIST`, 295,812 instructions to the ROM's
first `HALT` at `$0679`, no divergence. A taken `JR` planted at 13 T was
caught at the fourth instruction. xAce loaded an archive tape (`TutTut-122.zip`
from the Ace archive) through this ROM: `LOAD TUTTUT` gave
`Dict: TUTTUT     OK` (`out/m3-xace-tape.log`).

**xAce has no `.ace` loader**, so it cannot be the reference for snapshots.
MAME's `jupace` has one (`snapshot_cb`), but MAME is read here, not run.
Which reference checks `.ace` is M11's to choose (§18 item 6).

### 13.5 Soak

30 minutes on battery with a Forth program that prints, beeps and reads the
matrix itself while keys are typed over the UART. A script checks the log:
one boot, heartbeats throughout, real-time ratio ≥ 0.995, every failure
counter zero (EL §11.5).

---

## 14. Measuring

EL §12, with these workloads, scripted over the UART, one boot each:

| Workload | Why |
|---|---|
| idle at `OK` | the ROM's key wait, a spin rather than a `HALT` (§16) |
| compute: a tight Forth `DO … LOOP` with arithmetic | the inner interpreter |
| scrolling: `VLIST` repeated | screen writes and band presents |
| sound: `BEEP` in a loop | speaker edges |
| character-set animation | glyph-change dirty marking |

**Heartbeat**: real-time ratio, core 0 share and headroom, host cycles per
guest instruction, mean T per instruction, longest present, presents / full
presents / dropped snapshots, underrun samples, late refills, queue depth and
low water, samples consumed per second (**the control**, nominal
36,621.09 Hz), I²C errors, key events dropped, `ED` holes executed, log lines
dropped, battery and charging, die temperature.

---

## 15. Milestones

Sixteen milestones, each small enough to finish and check in a few sittings.
Each one:

- **ends with something that runs and something that is measured**, recorded
  here with the date, the board, and what was *not* verified (EL §14.3);
- has **done-when** criteria that can be checked by a test or on the panel,
  not "implemented";
- lists what it **leaves out**, so its scope does not drift into the next one.

"Built" and "done" are different words. A milestone with a device step is
done when it has been checked on the device.

### 15.1 The order

```
 host only                          device
 ─────────                          ──────
 M0 skeleton ──────────────────────────────────────────┐
  │                                                    │
 M1 Z80 on host ──────────► M2 Z80 on board (GATE) ─┐  │
  │                                                 │  │
 M3 Ace on host                    M6 bring-up ◄────┼──┘
  │  │                                  │           │
 M4 video   M5 keyboard                 │           │
  │          │                          │           │
  └──────────┴──────────────► M7 ACE BOOTS ON DEVICE ◄┘
                                        │
                     M8 audio ──► M9 card & ROM ──► M10 menu & tape trap
                                                         │
                                  M11 snapshots ◄────────┘
                                        │
                     M12 perf & soak ──► M13 signal tape ──► M14 wait states
                                                                  │
                                                         M15 finish
```

Host-only milestones (M3–M5) need no board and can run alongside M2 and M6.
**M7 is the one that matters**: everything before it is scaffolding, and
everything after it is the emulator getting better.

### 15.2 The milestones

#### M0. Skeleton

*Depends on:* nothing.
*Build:* repository layout (§4.1); CMake with the host build and a `pico2`
build; `src/core/config.h`; `test/host/test_util.h`; CI building both under
`-Wall -Wextra -Werror` and running CTest; `arm-none-eabi-size` printed;
version header from `git describe` at build time; firmware that prints a
banner (version, physical board, chip revision, clock) over UART1 and
blinks the status LED. Most of this is pico-atom's, renamed:
`CMakeLists.txt`'s shape, `cmake/version.cmake`, `ci.yml`, `tools/` and
`port/board.*` (§4.6).
*Done when:* a push builds both targets green in CI, a deliberate SDK
`#include` in `src/core/` fails the host build, and the banner appears in a
UART capture from a real board.
*Measured:* image size.
*Leaves out:* any emulation.

#### M1. The Z80 on the host

*Depends on:* M0.
*Build:* `src/core/z80.c` with every opcode, including undocumented ones (§5.1);
T-state accounting; INT and IM 0/1/2; `EI` delay; `HALT`; `R`; `MEMPTR`;
a flat 64 KiB test bus; the CP/M BDOS stub; `tools/fetch-test-suites.sh`.
*Done when:* ZEXDOC and ZEXALL run to completion with no errors; every FUSE
test passes on registers, memory, T-states and access order; the interrupt
tests pass (§5.4); a missing binary makes its test report skipped, not
passed.
*Measured:* ZEXALL wall time on the workstation (a regression marker only).
*Leaves out:* the Ace, the page table, `HALT` fast-forward.
*Done, 2026-10-03* (Apple M1 Pro, Apple clang 21; suites fetched that
day): all 1,356 FUSE tests pass on registers, `MEMPTR`, T-states, memory and
access order; ZEXDOC and ZEXALL pass all 67 groups each; the behaviour tests
pass, each with its control (§5.4). ZEXALL took 90.6 s in a Debug build and
26.4 s in Release, for 46.7 G T-states. CI ran the same suites green
for `633d1c0`, 2026-10-03. *Not verified:* per-access timing within an
instruction and contention, which are out of scope (§5.1); the CPU state in
locals (§5.2), left for M2 to measure.

#### M2. The Z80 on the board: the performance gate

*Depends on:* M1.
*Build:* a firmware image with only the Z80 and the flat test bus: a timed
ZEXDOC section and a Forth-shaped loop (an inner interpreter `NEXT` with a
few primitives, written in Z80 assembly), reporting host cycles per guest
instruction and mean T per instruction over UART.
*Done when:* both numbers are recorded at 150 MHz on one board, replacing
§3.2's estimate, and the **gate decision** is written into §3.2: 150 MHz
looks enough (projected core 0 share ≤ ~85 %, the Atom's shipped 2 MHz
figure), or which lever comes next. The deferred 300 MHz option is the last
of them (§18 item 3).
*Measured:* host cycles per guest instruction, flash vs. a first SRAM tier.
*Leaves out:* LCD, keyboard, audio.
*Done, 2026-10-03* (Plus 2 W, RP2350B rev 2, id `7458DC82A89AAC12`, at
150 MHz): `pico-ace-bench` runs the Forth-shaped loop and the start of
ZEXDOC (`src/bench/`), each checked first on the host by `test_bench`,
whose T-state and instruction counts the board reproduced exactly. 81.8 and
103.4 host cycles per instruction from flash, 82.8 and 86.0 with the
interpreter in SRAM (tier 2), at 7.82 and 8.09 T per instruction. The
projected core 0 share is 23–28 %, and the gate decision in §3.2 is that
150 MHz is enough. *Not verified:* the Ace's bus, interrupts and the real
ROM's mix (M3, M7); core 1 contending for the XIP cache (M7); CPU state in
locals, moved to M12 (§5.2).

#### M3. The Ace on the host

*Depends on:* M1. Uses the committed `roms/ace.rom` (§10.2).
*Build:* `src/core/ace.c`: the page table and every mirror (§6.1), the
three RAM configurations (§6.2), even/odd port decode (§6.5), the INT line
with a duration, `ace_run`/`ace_run_field` with debt carry and the field
split as runtime configuration (§11.1); the host harness that finds the ROM
by SHA-1 (pico-atom's `guest.c` adapted); a text dump of screen RAM; **xAce
built from its own checkout** with the trace tool started (§13.4).
*Done when:* the harness boots the real ROM to the `OK` prompt in every RAM
configuration and the dumped screen shows it; xAce runs the same ROM and
loads one archive `.tap`, and the first trace diff of boot
to `OK` is clean or its divergences are explained; the bus tests of §13.2 pass;
the build-time SHA-1 check refuses a corrupted copy of `roms/ace.rom`; **§16 is updated** with everything the
ROM settles (IM mode, RAM sizing and the unpopulated read, `HALT` use, RNG
seed, tape routine addresses), each with how it was settled.
*Measured:* T-states from reset to the first `OK`.
*Leaves out:* pixels, keys, sound.
*Done, 2026-10-03* (Apple M1 Pro, Apple clang 21). The `.ace` part of the
done-when was moved to M11 the same day (§18 item 6). The Ace
powers on to a blank screen with the cursor (`$97`) on the bottom line, not
to `OK`, which it prints only after a line runs. So the harness boots to
the cursor and then types `2 2 + .` into the matrix, and reads back
`2 2 + . 4  OK` on the top line, in all three machines (`test_boot`,
`out/m3-boot.log`). **Measured:** 86,272 T from power-on to the cursor in
the 3K machine, 88,192 in the 19K and 92,032 in the 51K (about 27 ms; the
difference is the RAM-sizing loop). The bus tests pass (`test_bus`), the
field tests pass with their controls (`test_field`), and the build refuses
a ROM with one byte changed or one byte short (`test_rom_embed`, shown to
fail with the SHA-1 check removed). §16 is updated with what the ROM
settled. xAce runs the same ROM, loads an archive `.tap`, and the trace
diffs of boot and of a typed line are clean (§13.4). CI ran green on both
jobs for PR #2, 2026-10-03, the trace diff included. *Not verified:*
anything on the device (the firmware does not link `ace.c` until M7, so
tier 1's placement is unchecked); the field's line numbers and INT timing
against the schematic; character RAM and open-bus read values.

#### M4. Video on the host

*Depends on:* M3.
*Build:* the row generator and LUT (§7.2), the glyph-change dirty bands
(§7.3), the frame pool (§4.4), the emulator's public-domain font as a
screen + character-set pair for its own pages (§7.5); PPM output.
*Done when:* golden images of the booted ROM, an inverse line, a redefined
character and the full glyph set are committed **after being looked at**;
the glyph-change test passes and its screen-only control fails; the frame
pool survives a long randomised interleaving; a power-on test pins what
zeroed character RAM shows.
*Measured:* nothing on hardware yet.
*Leaves out:* the LCD driver.
*Done, 2026-10-03* (Apple M1 Pro, Apple clang 21). `src/core/render.c`
is the row generator with its 4 KiB LUT and the glyph-change bands;
`snappool.c` is pico-atom's pool with the Ace's snapshot; `font.c` is §7.5's
font. Five golden images (§7.6) were looked at before they were committed,
and a one-pixel change to one of them fails `test_golden` at that pixel.
`test_render` brings a simulated panel up to date from the bands alone over
300 random edits, a third of them to the character set only, and matches a
full render each time; its control, the same run diffing the screen bytes
alone, leaves stale pixels as it must. A redefined `A` dirties exactly the
two cells showing it, one of them inverse. `test_snappool` runs 100,000
random transitions of both cores: core 0 always gets a buffer, never the one
core 1 holds, at most one is ready, and every publish is taken or counted
dropped. At power-on, zeroed screen and character RAM draw all paper, and
code `$80` over them a solid ink cell; the ROM has written the character
set by the end of the first field. CI ran green on both jobs for PR #3,
2026-10-03. *Not verified:* anything on the panel
(M7): byte order on the wire, the band present's timing, the pool under the
spinlock.

#### M5. The keyboard on the host

*Depends on:* M3.
*Build:* the matrix sweep (§9.3); `keymatrix.c` and `keymap_picocalc.c` adapted from pico-atom (§4.6), with the map as data (§9.2): held-key
set, canonicalisation, binding fixed at press, guest SHIFT and SYMBOL SHIFT
assertion, the Alt layer, paced replay into the matrix; the harness typing
through that path.
*Done when:* the sweep's result replaces §2.4 and is kept as a regression
test; every PicoCalc table entry types its character through the real ROM;
`2 2 + .` typed through the harness reads back the expected answer; the
static table checks (one binding per code, no swallowed chords) pass; the
minimum key hold is settled in §16.
*Measured:* fields per key the ROM needs.
*Leaves out:* the southbridge, game layouts.

#### M6. Board bring-up

*Depends on:* M0. Runs alongside M3–M5.
*Build:* pico-atom's drivers, copied and renamed (§4.6), brought up in HW
§10's order rather than written new: `southbridge`, `lcd` and the
corner-coded test pattern, the polled DMA blit, `kbd`'s FIFO drain into the
SPSC ring, `log`'s ring drained by core 1, core 1's loop with
`busy_wait_us_32` (never `sleep_us`), and UART bytes turned into key events.
The work is re-verifying on this tree, not re-deriving.
*Done when:* the test pattern shows correct orientation, colour order and
all four corners at 75 MHz SPI; every key press and release on the PicoCalc
is logged with its code; a key typed over the UART arrives as the same
event; a 10-minute run has zero I²C errors.
*Measured:* full-screen and 256×192 blit times; I²C transaction time.
*Leaves out:* the guest.

#### M7. The Ace on the device

*Depends on:* M2, M4, M5, M6.
*Build:* core 0 running `ace_run_field` paced on `time_us_64()` against an
absolute field deadline (no audio yet, EL §6.3); core 1 presenting
snapshots with dirty bands at (32, 64); the PicoCalc keyboard driving the
matrix; the ROM embedded in the image, with the build's SHA-1 check
(§10.2); a heartbeat.
*Done when:* the board powers up to `OK` on the panel; `2 2 + .` typed **on
the PicoCalc keyboard** prints the answer; arrows, DELETE and BREAK work; a
`VLIST` scrolls cleanly with zero dropped snapshots over 3,000 fields.
*Measured:* core 0 share and host cycles per instruction at the prompt, in
compute and in scrolling (§14); longest present.
*Leaves out:* sound, the card, the menu.

#### M8. Audio

*Depends on:* M7.
*Build:* pico-atom's `audio.*` and `beeper.*` (§4.6), which already
implement HW §5.3–5.4 (aligned ring, chained channels, both resets on
re-arm, `DMA_IRQ_0` at `0x40`, refill path in SRAM), the SPSC queue and the
box filter with its DC blocker. Here, the beeper runs at 6,656/75 T per
sample, it is driven from every even-port `IN` and `OUT` (§8), pacing moves
to the audio queue, and the two counters go on the heartbeat.
*Done when:* `BEEP` sounds at the pitch its loop count predicts; the host
edge-model test passes to 1 LSB; a 10-minute run reads 36,621 samples/s
consumed, with zero underruns and zero late refills; whether the ROM's key
scan clicks at the prompt is recorded (§8).
*Measured:* pitch against computed; samples/s control; core 0 share with
audio on.
*Leaves out:* volume and mute settings.

#### M9. The card

*Depends on:* M8.
*Build:* pico-atom's `sd`, `diskio` and `storage` (§4.6); the park/hand-off
at a field boundary feeding silence (§4.5), following pico-atom's `main.c`
but in its own file; `pico-ace.cfg` read at boot (read-only for now:
`ram`); card-detect handling.
*Done when:* the machine boots to `OK` with no card, with an empty card and
with a card holding `/ace/`; `ram = 3k` in the file boots a 3K machine; a
bad line is skipped and named on the heartbeat; a card change while parked
is handled without a hang.
*Measured:* boot time to `OK` with and without a card; card read time for
the settings file.
*Leaves out:* writing anything to the card.

#### M10. Menu and fast tape

*Depends on:* M9.
*Build:* the menu as a text page through the row generator (§12) with the
Tape page and a status row; Alt+M, F1 and pause; the phase-1 tape trap
(§10.3) for `LOAD`/`BLOAD`/`VERIFY`/`BVERIFY` from `.tap` files and
`SAVE`/`BSAVE` appending through `.new` and rename.
*Done when:* a `.tap` from the archive `LOAD`s and runs; a word `SAVE`d on
the device loads back after a power cycle; a request no file answers falls
through to the ROM's own routine; the host test shows the trapped call
leaves the same RAM as the ROM's routine; no underruns during any load.
*Measured:* load time of a 16 KiB program; underruns during card work.
*Leaves out:* signal-level tape, recording to new image formats.

#### M11. Snapshots

*Depends on:* M10.
*Build:* `.ace` import (§10.5) with RAM-size refusal; `.sav` save and load
with two-pass load; the Snapshot page (F2).
*Done when:* a reference emulator chosen here loads the same archive
`.ace` (moved from M3, §18 item 6); `.ace` files from the archive load and run in the matching RAM
configuration and are refused in the wrong one, naming the size needed; the
host `.sav` round trip (150 fields identical) passes; a torn or foreign
`.sav` leaves the running machine unchanged.
*Measured:* snapshot load time.
*Leaves out:* `.ace` export (§18).
*Open:* the reference emulator that checks `.ace` import. xAce has no
loader (§13.4); MAME's `jupace` has one but is not run here (§18 item 6).

#### M12. Performance pass and soak

*Depends on:* M11.
*Build:* the scripted workloads (§14); `HALT` fast-forward if `VLIST` and
programs show it pays (M3 found the ROM halts in `VLIST`, not at the prompt); SRAM placement tiers, each in its own build directory with
symbols checked (HW §9.8); the computed-goto experiment and CPU state in
locals, both optional after M2's margin (§3.2, §5.2).
*Done when:* every change is measured against a control build in the same
sitting and kept only if it pays; §3.2 records the final numbers; a
30-minute battery soak passes (§13.5) with every counter zero.
*Measured:* each workload's core 0 share before and after; soak log.
*Leaves out:* new features.

#### M13. Signal-level tape

*Depends on:* M12.
*Build:* `.tap` blocks decoded to half-cycles in T-states on the input bit
with a next-edge countdown (§10.4); the motor following the ROM's cues;
recording from the output bit; turbo while the tape plays.
*Done when:* a `.tap` loads through the ROM's own routine with the trap
disabled; the host round trip (ROM `SAVE` → independent decoder → `.tap` →
ROM `LOAD`) passes; a recording made on the device loads in xAce.
*Measured:* turbo load speed-up; core 0 share while a tape plays.
*Leaves out:* `.wav`, `.tzx` (§17).

#### M14. Wait states

*Depends on:* M13, and the trace-diff tool kept working since M3 (§13.4).
*Build:* a measurement of the ROM printing a
screenful and of a game's frame loop, with and without a wait-state model
in the slow path.
*Done when:* §6.4 records the measured difference and the decision (model
it or not), and if modelled, the cost against a control build is within
what §3.2 leaves.
*Measured:* wait-state impact on guest timing; its host cost.
*Leaves out:* display snow (§17).

#### M15. Finish

*Depends on:* M14.
*Build:* game layouts, built-in and from the card (§9.4); settings saved by
editing in place (§10.6); the Machine page with staged changes and power-on
restart; the About page; the release build without UART; the README (the
ROM's provenance and permission, card layout, keys).
*Done when:* every menu row works on the device; a settings save keeps the
user's comments and order and a second save changes nothing; the soak
passes again on the release build (counters read over SWD).
*Measured:* the release build's soak; die temperature on battery.
*Leaves out:* everything in §17, including the 300 MHz host clock and
`.ace` export (§18).

---

## 16. Unverified constants

Every guest fact the code depends on, its best primary source, and its
confidence. Each is transcribed from its source, or settled by executing the
ROM, before it becomes a `#define`. While unverified, a timing constant is
runtime configuration (EL §14.2).

| Constant | Believed | Primary source | Confidence |
|---|---|---|---|
| CPU clock | 3.25 MHz | schematic (crystal, divider) | high |
| T-states per line | 208 (416 pixel clocks ÷ 2) | schematic; MAME `jupace` | medium. MAME's source read 2026-10-03: `set_raw(6.5_MHz_XTAL, 416, …, 312, …)`. Not yet against the schematic |
| Lines per field | 312 | schematic; MAME | medium. MAME agrees (above); FRAMES counts one a field under it (`test_field`) |
| First active line, INT line | 56 and 248 | schematic; MAME; trace diff | medium-low. MAME's (192 lines drawn from 56; INT set at line 248), read 2026-10-03, are `ace_config_default`'s, still runtime configuration. A redraw after INT is finished at line 56 (`test_field`) |
| INT duration | 8 lines, 1,664 T | schematic (the INT generator) | medium. MAME clears INT at line 256. **Bounded by the ROM**, 2026-10-03: its handler opens with a ~800 T delay and reaches `EI` at `$017C` 1,819 T after INT rises at idle, so INT held much past ~1,800 T would be taken twice; FRAMES (`$3C2B`) counts once a field at 1,664 T and twice at 2,500 (`test_field`) |
| IM 2 vector byte (bus float on acknowledge) | `$FF` | schematic | low |
| ROM size and hashes | 8,192 bytes; SHA-1 `597ba8a1…` (§10.2) | `roms/ace.rom`, hashed 2026-10-03 | **settled** 2026-10-03: both halves' CRC32 and SHA-1 match `ROM_LOAD` in MAME's `src/mame/cantab/jupace.cpp` |
| Video and character RAM mirrors | §2.2 | schematic; ROM's own addresses | medium-high. The ROM writes the screen at `$2400`, workspace at `$2700`, the character set at `$2C00` and keeps its variables at `$3C00`, and boots in all three machines with §6.1's table (`test_boot`, 2026-10-03). MAME's map agrees, but reads character RAM back |
| Which mirror waits, and for how long | `$2400`/`$2C00` wait during active display | schematic | medium / low |
| Character RAM read value | `$FF` (`cram_read`) | schematic | low. Runtime configuration. MAME reads it back as RAM, xAce too |
| Unpopulated read value | `$FF` (`open_bus`) | schematic; ROM's RAM sizing | low for the value; **what the ROM needs is settled**, 2026-10-03: its sizing at `$0028` writes `$FC` a page at a time from `$3D00` and stops at the first page that does not read it back, so any value but `$FC` works. RAMTOP (`$3C18`) is `$4000`, `$8000` and `$0000` in the three machines (`test_boot`) |
| User RAM mirrors with a pack fitted | still mirrored at `$3000–$3BFF` | pack schematic | low |
| Port decode | A0 only | schematic; ROM | medium |
| Keyboard matrix | §2.4 | **ROM, executed** | medium. MAME's table agrees with §2.4. The cells typed through the ROM so far (letters, digits, SPACE, ENTER, SHIFT, SYMBOL SHIFT with K and M) all type what §2.4 says, 2026-10-03. The full sweep is M5's |
| Port read bits D5–D7 | tape on D5, rest high | schematic; ROM's tape loader | low-medium. MAME's `io_r` agrees: `$FF`, D5 cleared by the tape signal |
| `IN` vs `OUT` speaker direction | `IN` one way, `OUT` the other | schematic; ROM's `BEEP` | medium. MAME: `IN` low, `OUT` high, which `ace.c` follows. MAME also takes the tape output from **D3 of the `OUT`**, not from the access, which contradicts §2.3; settle in M13. The prompt makes no edge: its key scan is all `IN`s (`test_boot`) |
| Display polarity | set bits white | ROM, executed, against photographs | high. **Executed** 2026-10-03: with set bits as ink, the character set the ROM writes reads as text on a paper ground that its spaces clear to (`test/host/golden/boot.ppm` and `glyphs.ppm`, looked at). That paper is black and ink white is from photographs |
| ROM uses IM 1 | yes | ROM | **settled** 2026-10-03: `IM 1` at `$008E`, `EI` at `$009F`; IM is 1 at the prompt in every machine (`test_boot`). The handler is at `$013A` |
| ROM halts when waiting for a key | no | ROM | **settled** 2026-10-03, by execution: at the prompt it spins on FLAGS (`$3C28`) bit 5 at `$059B`, which the interrupt sets on ENTER, and never halts (`test_boot`). It does `HALT` once a word in `VLIST` (`$0679`), found by the trace diff |
| RNG seed location | none | ROM | **settled** 2026-10-03: the ROM has no random-number word (its dictionary names were listed). The manual's `RND` keeps its own seed and seeds it from FRAMES (`$3C2B`), which the interrupt counts, so zeroed RAM leaves nothing stuck (§6.3) |
| Key minimum hold, in fields | 3 scans | **ROM, executed** | partly read 2026-10-03: the interrupt's scan (`$0310`) counts a held key down from `$20` in `$3C27`, takes it on the third consecutive field that sees it, repeats it 30 fields later and then every 4. A key held 4 fields with 4 between types once each (`test_boot`). M5 settles the hold and gap the replay uses |
| `.tap` block layout | §10.3 | ROM tape routines; archive files | medium-low |
| `.ace` snapshot encoding | §10.5 | xAce/EightyOne docs; sample files | low |
| Tape signal timings | — | ROM tape routines | unknown |
| Tape block routines | load `$18A7`, save `$1820` | ROM | medium: xAce patches the ROM at these two addresses, and with them loaded an archive `.tap` through this ROM, 2026-10-03 (`out/m3-xace-tape.log`). Read the routines in M10 before trapping them (§10.3) |

Record how each was settled, and the date, in this table when it changes.

---

## 17. Dropped and deferred

Each entry says why, so nobody re-plans it without new evidence (EL §14.5).

| Feature | Decision | Why |
|---|---|---|
| RP2040 boards | dropped | memory would fit (§3.3), but the Z80 estimate already needs most of an M33 core at 150 MHz; an M0+ cannot do it in real time |
| Display snow from the CPU-priority mirrors | dropped | authentic but ugly, and needs per-T-state beam position; no software is known to rely on it |
| Wait states on the waiting mirrors | deferred to M14 | measure the effect first (§6.4) |
| Scaled display (320×240) | dropped | 56 % more wire for an uneven stretch (EL §5.4) |
| Colour themes (green, amber) | dropped | the Ace is white on black; a pointer swap if ever wanted (§7.2), not worth a menu row now |
| Ace sound boards (AY-3-8912 add-ons) | deferred | small user base; would be the first odd-port device and a PSG synth (HW §5.5). Revisit if the archive search shows titles that need one |
| Printer, ROM expansions, Boldfield add-ons | dropped | no known software dependency |
| Faster guest clock | dropped | no common modification to support; leaves §11.2 trivial |
| `.wav` and `.tzx` tapes | dropped | the archive is `.tap` and `.ace`; a user can convert on a computer |
| The Ace DOS ROM (`roms/JA-DOSROM/`) | dropped | the Ace has no disc in this design, and no permission covers that ROM. It stays out of the repository (`.gitignore`) |
| 300 MHz host clock | deferred | 150 MHz is the target. Taken back up only if M2's or M7's core 0 share fails the §3.2 gate after the other levers. pico-atom's clock code is ready if it is (§18 item 3) |
| `.ace` export | deferred | import covers the archive, and `.sav` covers our own saves. Export waits until someone asks (§18 item 4) |
| Hardware vertical scroll | dropped | the Ace scrolls by memory moves the dirty diff already catches (EL §5.4) |

---

## 18. Decisions

The owner's decisions, each with its date. None is open as of 2026-10-03.

1. **Default RAM size: 19K** (stock + 16 KiB). *Decided 2026-10-03.* Most of
   the archive needs it, and the stock 3K and the 51K are on the Machine page
   (§6.2).
2. **The ROM: shipped, embedded in the firmware.** *Decided 2026-10-03,
   replacing the same day's "users supply their own".* The basis is
   `roms/COPYING.md`: a 1998 email from Paul Downham of Boldfield Computing,
   which bought Jupiter Cantab's remaining stock and assets, to Edward
   Patel, author of xAce. It says "I am sure nobody is going to get upset
   about the emulator", and that the ROM listing had been given away. It is
   informal, it was addressed to another emulator's author, and it does not
   show that the ROM's copyright passed to Boldfield. It is, however, what
   Ace emulators have relied on for over 25 years. The ROM is not under this
   project's GPL. `THIRD-PARTY.md` says so and points to `COPYING.md`. If a
   rights holder objects, the fallback is the earlier plan: the ROM on the
   card, identified by SHA-1, with a missing-ROM page (EL §8.1).
3. **300 MHz host clock: deferred.** *Decided 2026-10-03.* The emulator is
   built for 150 MHz only. The 300 MHz option comes back into the plan only
   if M2's projection or M7's measurement of core 0 is over the ~85 % gate
   after the other levers (§3.2). It would then be pico-atom's
   `board_init_clocks()` path and a `host_mhz` setting (§17).
4. **`.ace` export: deferred.** *Decided 2026-10-03.* `.ace` import and our
   own `.sav` states are in the plan (§10.5, M11). Writing `.ace` files for
   other emulators waits until someone asks (§17).
5. **Licence, and so reuse: GPL-3.0** (`LICENSE`). *Decided 2026-10-03.*
   The same as pico-atom, so §4.6's files move across as they are. Each one
   brings its `THIRD-PARTY.md` entry with it (ClockworkPi's LCD init values
   in `lcd.c`, FatFs's `ffconf.h`).
6. **Snapshots wait for their milestone; M3 needs no `.ace` reference.**
   *Decided 2026-10-03.* M3 found that xAce has no `.ace` loader (§13.4).
   Snapshots are not a critical feature, so the check that a reference
   emulator loads an archive `.ace` leaves M3's done-when, and choosing a
   reference for `.ace` is left to M11 (§15.2), which builds the import.

---

## 19. Sources

To obtain and record (with revision or date) before transcribing constants:

- **Jupiter Ace User Manual**, *Jupiter Ace FORTH Programming* (Steven
  Vickers, Jupiter Cantab, 1982): keyboard, editing keys, tape words, memory
  map, `BEEP`.
- **The Ace schematic**: clock, video timing, INT generator, mirrors, wait
  logic, port decode, bus pull-ups.
- **A commented ROM disassembly**, and **the ROM itself, executed on the host
  harness**. This is the best source for the matrix, tape routines, key timing
  and system variables (EL §14.2).
- **MAME `jupace` driver**: the ROM halves' hashes to confirm `roms/ace.rom` against (§10.2), and a second opinion on the memory map and video
  timing. Read it, copy nothing.
- **xAce** (Edward Patel) and **EightyOne**: the reference emulator for trace
  diffs (§13.4), and the `.tap`/`.ace` format definitions.
- **Zilog Z80 CPU User Manual** and **"The Undocumented Z80 Documented"**
  (Sean Young): instruction timing, undocumented flags, `MEMPTR`, interrupt
  behaviour.
- **ZEXDOC/ZEXALL** (Frank Cringle) and the **FUSE Z80 test suite**: fetched,
  not committed (§5.4).
- **The Jupiter Ace Archive** (community site): software, tapes, snapshots,
  and the evidence for §17's archive questions.
