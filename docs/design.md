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

**Status, 2026-10-04.** M0 to M9 are done (§15.2): the Z80 and the Ace
on the host and on the board, video, the keyboard, audio and the card's
settings file, each with what was checked on the device recorded under its
milestone. Every number about the Ace below comes from secondary knowledge
until §16's table says otherwise. Every performance figure is an
**estimate** and is labelled as one (EL §14.4) until a milestone measures
it on the board; the measurements are recorded under their milestones and
labelled as measured where they replace an estimate, as in §3.2.

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

### 2.4 Keyboard matrix

**Settled by executing the ROM** (§9.3), 2026-10-03: each of the 40 cells
was pressed at the prompt alone, with SHIFT, with SYMBOL SHIFT and with
both, and what the ROM typed was read back. `test_keyboard` keeps that
sweep as a regression test.

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

Letters type lower case at power-on and capitals with SHIFT. SYMBOL SHIFT
gives `:` `£` `?` on Z X C; `~` `|` `\` `{` `}` on A–G; `<` `>` on R and T
(Q, W and E give capitals); `!` `@` `#` `$` `%` on 1–5; `_` `)` `(` `'` `&`
on 0–6; `"` `;` `©` `]` `[` on P–Y; `=` `+` `-` `^` on L–H; and `.` `,` `*`
`/` on M–V. SHIFT with SYMBOL SHIFT types what SYMBOL SHIFT alone does.

The editing functions are SHIFT with a digit: 1 DELETE LINE, 2 CAPS LOCK,
4 INVERSE VIDEO (a toggle), 5 cursor left, **6 up, 7 down**, 8 right,
9 GRAPHICS, 0 DELETE. **SHIFT+3 types `3`**: this ROM has no TRUE VIDEO
key, and INVERSE VIDEO pressed again turns inverse off. SHIFT+SPACE is
BREAK, which a running word tests for (it stops with `ERROR 3`); at the
prompt it types a space. The scan's decoded key is at `$3C26` while the
key is down: `$01` left, `$02` CAPS LOCK, `$03` right, `$04` GRAPHICS,
`$05` DELETE, `$07` up, `$08` INVERSE VIDEO, `$09` down, `$0A` DELETE
LINE, `$0D` ENTER.

Before the sweep this section had 6 as down and 7 as up, and SHIFT+3 as
TRUE VIDEO, from secondary sources.

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

**Measured, M7, 2026-10-03** (the same board, 150 MHz, gcc 15.2, the whole
machine: the real ROM in the 19K Ace, paced on `time_us_64()`, core 1
presenting and polling the keyboard). Each workload typed over the UART
after a fresh boot; figures are core 0's heartbeat over 5 s windows, which
agreed to within 0.5 %. Logs: `out/m7-*.log`.

| Workload | T per insn | Host cycles per insn, tier 0 | tier 1 | tier 2 | Core 0, tier 0 | tier 2 |
|---|---:|---:|---:|---:|---:|---:|
| idle at the prompt | 11.9 | 110.3 | 108.8 | | 20.1 % | |
| compute: `: c 0 30000 0 do i + loop drop ;` repeated | 9.52 | 159.6 | 161.3 | 89.4 | 36.4 % | 20.4 % |
| scrolling: `vlist` repeated | 4.26 | 75.6 | | 71.2 | 38.6 % | 36.2 % |

The real machine costs more than the bench: compute at tier 0 is about
twice the Forth-shaped loop, so the bench's projection of 23–28 % was low,
but the worst case, 38.6 %, is under half the ~85 % gate. **Tier 1 gained
nothing**; tier 2 took compute to 89 cycles, near the bench, which is
the XIP cache shared with core 1 that the bench left out. Scrolling's
figures are distorted by `HALT`: `VLIST` halts once a word (§5.3), and each
repeat of `HALT`'s NOP counts as an instruction, hence 4.26 T. Most of its
core 0 time is probably those NOPs, which is the case for `HALT`
fast-forward. M12 chooses the tier that ships and measures fast-forward.

**Measured, M8, 2026-10-04** (the same board, tier 0, paced on the audio
queue, against a `PICO_ACE_AUDIO=OFF` control flashed in the same sitting).
The workloads ran as loops, `: r begin c 0 until ;` and `: v begin vlist 0
until ;`, rather than retyped, so they differ from M7's figures; compare
within a row. Logs: `out/m8-*.log`.

| Workload | Core 0, audio | control | Host cycles per insn, audio | control |
|---|---:|---:|---:|---:|
| idle at the prompt | 21.2 % | 20.8 % | 116.2 | 114.2 |
| compute, `c` in a loop | 42.2 % | 40.2 % | 185.1 | 176.2 |
| scrolling, `vlist` in a loop | 40.8 % | 39.5 % | 79.9 | 77.2 |

Audio costs 0.4–2.0 points of core 0. Most of it is in the guest's own
cycles per instruction, not in the beeper's few calls a field, which
suggests the refill IRQ and `audio_push` (in flash at tier 0) taking XIP
cache lines from the interpreter. M12 measures that with the tiers.

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
| Log ring | 4 K | EL §2.3. As built in M6: a 2 KiB ring (`ACE_LOG_RING`) and a 512 B line on core 0's stack |
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

**Changed in M10, 2026-10-04, at the owner's request:** the 8×8 font was
hard to read on the panel, so the pages now use **the Ace's own character
set, expanded from the embedded ROM** (`font_from_rom`, `romfont.c`), as its
power-on code at `$0052`-`$008D` writes it: the 32 block graphics worked out
from their codes, the 95 printable glyphs from a table read down from
`$1FF3` (a blank top row, then seven rows, or six and a blank bottom row
when bit 5 of the count is set), and the copyright sign from `$1FF4`. It
still does not depend on character RAM. `test_boot` requires it to equal
the character RAM the ROM writes, in all three machines, and a planted bug
fails that. The menu, the PAUSED line and the perf line use it, and the menu
is in mixed case. `font8x8` is no longer in the firmware; it stays as the
font of `test_golden`'s `font.ppm` and `test_render`'s checks.

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

**Found by executing the ROM, 2026-10-04** (`test_audio`, M8):

- **The prompt is silent.** The key scan is all `IN`s, so after the first
  the speaker stays low; typing a key makes no edge either. The ROM does not
  click.
- **`BEEP ( m n -- )`** runs its loop at `$0BAF` with interrupts off: `IN`
  (speaker low), a delay, `OUT` (speaker high), the same delay. Each half is
  49 T plus the delay routine at `$0BC9`, which is 13m − 47 T, so a half
  period is **13m + 2 T** and a period **26m + 4 T**: 8m µs and 4 T, the
  manual's figure. The count holds for m ≥ 6 with (m + 249) & `$FF` ≠ `$FF`,
  because the delay increments only the low byte. Executed at m = 50, 100 and
  300, every half period is the count to the T-state, and the pitch measured
  off the output is the count's to 1 part in 10⁴: 2,492.33, 1,248.08 and
  416.45 Hz. The loop reads SPACE alone to stop a note (ERROR 3).
- A note ends on its `OUT`, so the speaker is left high until the next key
  scan's `IN`; the DC blocker makes that inaudible.
- The edge is stamped with `cpu.t` at the access. The Z80 adds an
  instruction's T-states after its bus accesses, so that is the
  instruction's start, as EL §6.1 asks, with no extra store per
  instruction. A stamp at the instruction's end fails the 1 LSB check by
  2,031.

---

## 9. Keyboard

### 9.1 The path

EL §7.1's backwards mapping: PicoCalc `[state, code]` events → normalised →
held-key set → binding fixed at press → (row, col, guest SHIFT, guest SYMBOL
SHIFT) → the matrix, **replayed at the guest's pace** (each key held a minimum
number of fields with a gap after). The ROM's scan (`$0310`) takes a key on
the third consecutive field that sees it, needs one field with every key up
before the next, and repeats a key held 33 fields. The replay holds each key
4 fields and leaves 2 up (`ACE_KEY_MIN_FIELDS`, `ACE_KEY_GAP_FIELDS`), one
more of each than the ROM needs, for a scan the guest delays: 6 fields a
key, ~8 keys a second (`test_keyboard`, 2026-10-03). Shift and Ctrl reach
the matrix, so their releases wait the same minimum: a tap that starts and
ends inside one poll still reaches a game that reads SHIFT alone. Insert is
both Shift+Enter and Alt+I (HW §6.3), so which key it belongs to is decided
when its event arrives, by whether Alt is down then.

### 9.2 The standard map

The map is data, one row per PicoCalc code (EL §7.2):

| PicoCalc | Ace | Note |
|---|---|---|
| `a`–`z`, `0`–`9` | the key | |
| `A`–`Z` | SHIFT + key | the PicoCalc's own Caps Lock works through this |
| punctuation | SYMBOL SHIFT + the key §2.4's sweep found | the shifted-only characters (`\|` `{` `}` `~`) have entries of their own (EL §7.2). The Ace has `£` where ASCII has `` ` `` (`$60`, §7.5), so `` ` `` types `£`; `©` has no PicoCalc key and is Ctrl+I |
| Enter, Space | ENTER, SPACE | |
| Backspace, Del | DELETE (SHIFT+0) | |
| ← ↑ ↓ → | SHIFT+5 / 6 / 7 / 8 | guest SHIFT is ours, so the swallowed host Shift+arrow chords cost nothing (HW §6.3). Up is 6 and down 7 (§2.4) |
| Esc, Break | BREAK (SHIFT+SPACE) | the host's Shift+Space never arrives (HW §6.3), so BREAK needs a plain key. Break is Shift+Esc |
| **Shift** held | asserts guest SHIFT, **except while a key it shifted on the PicoCalc is down that the Ace types without SHIFT** | games read SHIFT alone (EL §7.2). The exception is pico-atom's `unshift` flag: on the PicoCalc `:` `"` `!` and the like are Shift chords, but on the Ace they are SYMBOL SHIFT chords. The ROM types the same with SHIFT down as well (§2.4), so the exception keeps the matrix as an Ace typist would leave it, for a program that reads it directly |
| **Ctrl** held | asserts guest SYMBOL SHIFT | Ctrl+key reaches every symbol-shifted cell raw, which the MCU delivers unchanged (HW §6.3) |

**Alt layer** (the Ace has no Alt, so nothing is stolen from it; a key with
Alt down comes from this layer only):

| Chord | Action |
|---|---|
| Alt+L | CAPS LOCK (SHIFT+2) |
| Alt+G | GRAPHICS (SHIFT+9) |
| Alt+V | INVERSE VIDEO (SHIFT+4), a toggle; the ROM has no TRUE VIDEO (§2.4) |
| Alt+X | DELETE LINE (SHIFT+1) |
| Alt+M | menu |
| Alt+P | pause |
| Alt+R | reset (asks first) |

Avoid Alt+`,` `.` Space `B` (MCU's own) and Alt+I (the MCU's Insert) (HW
§6.3). F1–F5 and F10 open emulator pages directly (§12).

### 9.3 Settling the matrix by execution

In M5, on the host: boot the ROM, then press each of the 40 cells at the
prompt alone, with SHIFT and with SYMBOL SHIFT, and read what the ROM puts in
screen RAM. That gives the whole map, including the editing keys and every
punctuation character's cell. Keep the sweep as a regression test that types
every entry of the PicoCalc table through the real ROM (EL §7.2). A host test
also checks that every code maps to exactly one binding and that no binding
needs a chord the MCU swallows.

Done 2026-10-03 (`test_keyboard`, `test_keymap`). The sweep, with both
shifts as a fourth case, gave §2.4. Every printable entry types its own
character through `keymatrix` and the ROM; the editing keys, the Alt layer
and BREAK are checked by what they do to the input line or a running word.
Two of §2.4's earlier beliefs were wrong: up and down were swapped, and
SHIFT+3 is not TRUE VIDEO.

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
  states/slotN.sav      our own save states, four slots (§10.5)
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
- **Format: `.tap` as the Ace community uses it**, settled against the ROM
  and an archive file (§16): each block is a 2-byte little-endian length,
  the block's bytes, and their XOR. The flag byte that precedes a block on
  tape is not stored, so a block's flag is its place: even blocks from the
  start of the file are headers, odd ones data.
- `SAVE` appends a header and data block to the selected tape image through
  `.new` and rename (EL §8.6).

**As built (M10).** The trap is on the two block routines, `$1820` and
`$18A7`, and the words around them (finding a name, printing it, checking
the length) stay the ROM's: it calls again for the next block, as it would
with a recorder. The CPU offers a trapped PC to the bus before running it,
in a second loop of `z80_run` used only when traps are set (`z80.h`). The
trap stands aside unless the ROM's bytes from `$1820` to `$192C` are the
stock ROM's. A served block does not return by itself: the trap puts the
machine where the ROM's routine is as it finishes reading the last byte it
would have taken (`$190C`), or sending the checksum (`$1872`), with the
stack the routine leaves, and the ROM runs its own last instructions: the
checksum test, the flag mismatch, a verify's difference, the BREAK check,
`EI` and the `RET`. So what the routine leaves is the ROM's own work, and
`test_tape` holds the rest to the ROM's routine fed its own recorded
signal (§16). Two deliberate differences: R, which counts instructions the
trap does not run; and a block shorter than the ROM asks for, which is
taken as a time-out where a tape would go on to read the next leader as
bytes.

The deck (`tapeio.c`) holds one tape and reads it a block at a time from
where it stands. With the deck empty, `LOAD SQ` plays `/ace/tapes/SQ.tap`
and `SAVE SQ` appends to it, creating it; a `LOAD` whose name is a file on
the card plays that file whatever is in the deck. A load that reaches the
end of the tape rewinds once, so a program already passed is found; at the
end a second time it is declined, and the ROM waits for a signal until
BREAK, as the real machine does. Archive files are seldom named after the
program they hold (`tut-tut.tap` holds `TUTTUT`), so with the deck empty
and no file of the name asked for, the first tape whose first header
carries that name is played. The Tape page skips the `._` files macOS
writes beside every file it copies.

### 10.4 Tape, phase 2: the signal

The ROM reads the tape bit with its own timing loops. Decode `.tap` blocks to
**half-cycles clocked in T-states**, presented on the input bit and brought up
to date only when the port is read, with a next-edge countdown (EL §4.3,
§8.3). This loads protected or custom loaders and makes turbo free. Read the
ROM's routines first for leader, sync and bit timing, and carry remainders so
half-cycles never drift. Recording decodes the output bit back into blocks.
`.wav` is out of scope (§17).

### 10.5 Snapshots

**`.ace` (import only; export is deferred, §18 item 4).** This is the
archive's common format, ACE32's, also written by EightyOne and read by MAME.
Settled 2026-10-04 (§16) from the Jupiter Ace Archive's description of it
(`faq_ace_snapshot_format.html`, from Edwin Blink's study of ACE32's files),
MAME's loader (`jupace.cpp`, `snapshot_cb`, at `774a180`) and 199 files:
the archive's four (Dreamsoft, in `ace-pack-2-tzx-047-050.zip`) and the 195
in TOSEC's Jupiter Ace set (2012-04-23, on archive.org). `snap_ace.h` has
the format:

- The address space from `$2000` up, run-length encoded: `ED 00` ends it,
  `ED n b` is n copies of b, any other byte is itself. All 199 files end
  with `ED 00` and nothing after it.
- In the screen's undisplayed mirror at `$2000–$23FF`, ACE32's state as
  32-bit words: at `$2080` the machine's RAMTOP (`$4000` 3K, `$8000` 19K,
  `$C000` 35K, `$0000` 51K), and from `$2100` AF BC DE HL IX IY SP PC AF'
  BC' DE' HL' IM IFF1 IFF2 I R. Only the low 16 bits of a pair's word and
  the low byte of the others mean anything: the files hold noise above
  them (one IM word reads `120FE701`).
- **No file dumps past `$7FFF`.** They decode to 8 KiB or 24 KiB whatever
  their RAMTOP, six 3K files ending at `$8001` with `$07` padding. So the
  TOSEC tag (`[3K]`) is the RAM the program needs, not the machine it was
  saved on: 76 of its 3K programs were saved from 19K machines.
- By RAMTOP the files were taken on 3K (42), 19K (118), 35K (36) and 51K
  (3) machines. A file from a 35K or 51K machine lacks that machine's top
  of RAM, which holds the Z80's stack.
- **The key wait's stack is one word.** Of the 135 files saved in the
  ROM's key wait (`$059B: BIT 5,(HL)` with HL = FLAGS, `$059D: JR Z`) whose
  stack is in the file, every one has SP = RAMTOP − 2 and `$04F7` there,
  and the host ROM's prompt has the same (`test_snap_ace`).

The import writes the screen, character set, user RAM and pack from their
own addresses and not from the mirrors, sets the registers, and restarts
the field. **The machine a file needs is its RAMTOP's**, and any other
refuses it, naming the one it needs. **A 35K file loads into the 51K
machine** (§18 item 7), there being no 35K one. When a file's stack top is
not in it, the key wait's `$04F7` is written back at RAMTOP − 2 if the file
is in the key wait with HL = FLAGS and SP = RAMTOP − 2; otherwise the file
is refused. Of the 199 files, 198 load (35 with the word written back) and
one, Ace Invaders (1982, Hi-Tech), is refused: it was saved running, from a
51K machine, with its stack top at `$FFF8`. Turbo and Valkyr, also saved
running from 35K and 51K machines, have their stack top in the file and only
the older frames outside it: they load, and would meet the missing frames
only on returning to Forth, as they do in MAME.

**`.sav` (our save states).** EL §8.5 exactly, as pico-atom's `snapshot.c`
with the Z80's and the Ace's fields (`snapshot.h`): explicit little-endian
fields, magic `PACESNAP`, version, lengths, CRC-32, reserved bytes zero,
the ROM's SHA-1 and not its bytes, the RAM size, field shape and bus values
recorded and checked, and a two-pass load. States are taken between fields,
where the guest is parked, so the field resumes from its first active line
and the budget carries the overshoot. The beeper's sample grid is not
state: audio restarts from the restored T counter, so the speaker's edges
are the same and the samples within one of them (`test_snapshot`). Four
slots on the card, written through `.new` and a rename, a whole `.new`
taken when the slot's file is missing or damaged (EL §8.6).

### 10.6 Settings

`/ace/pico-ace.cfg`, `key = value`, edited in place, parsed back before writing,
saved only by a menu action, no flash writes (EL §8.7). Keys: `ram`
(`3k`/`19k`/`51k`, default `19k`), `volume` (0–8, default 8), `layout` (a
layout's name, at most 16 characters, or `standard`, the default),
`boot_tape` (a path, or a bare name in `/ace/tapes/`; empty is none) and
`perf_line` (`on`/`off`, default `off`). Names and values are read in either
case, and a `#` at the start of a line or after a space begins a comment.
Build-time `BOOT_*` overrides win (EL §13.1); M9 has `PICO_ACE_BOOT_RAM`.

The parser and the in-place rewriter are pico-atom's `settings.*`, with
these keys (§4.6), and `test_settings` came with them. M10 changed one rule:
a refused value is replaced by the value in force when no other line gives
that key a good value, and made a comment when one does, so that saving
clears the problem the status row names. Core 1 reads the
file at boot, before core 0 powers the machine on, because `ram` is the
machine. As of M9 only `ram` is applied; the other keys are read and
checked, so a mistake in them is named, and M10 applies them.

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

- **Boot straight to the prompt**: the Ace's blank screen and cursor,
  with no menu or splash first (the ROM prints `OK` only after a line
  runs, §15.2 M3).
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

**As built (M10).** The menu (`menu.c`) runs on core 1 with the guest
parked (§4.5), and owns the keyboard while it is open: core 0 leaves the
key ring to it. Its pages are the main page (Tape, Settings, Save settings,
Reset), Tape (empty the deck, rewind, the files in `/ace/tapes/` with each
one's first header name) and Settings (volume, perf line). F2-F5 open the
main page saying the page is not in this firmware yet; Snapshot is M11's.
**M11** added the Snapshot page (F2, and from the main page): a slot chosen
with < >, then Save, Load and Delete for `/ace/states/slotN.sav`, and the
`.ace` files in `/ace/snaps/`. A load that succeeds closes the menu and
the guest resumes from it; a refusal is named on the status row, an `.ace`
for another machine as the machine it needs.
The pages are in mixed case, in the Ace's own character set (§7.5).
What the menu changes for core 0, the volume and a reset, goes through
`g_ui` and is applied by core 0 when it has the machine back (EL §2.5).
Alt+R resets the CPU with RAM kept. Save settings writes the running
machine's RAM size, the volume, the perf line and the tape in the deck, if
the user put it there, as `boot_tape`. Over the UART, RS opens the menu
and US pauses, and while either is up the UART's bytes are its keys, with
^P ^N ^B ^F for the arrows (`park.c`).

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
**MAME is the reference for `.ace`** (§18 item 6), run headless:
`tools/ace-reference.py` has MAME 0.289's `jupace` (16K fitted) load each
19K archive file and run 100 fields, has `test_snap_ace` do the same, and
compares the registers, the screen and `$3C00–$7FFF`
(`tools/mame/ace-dump.lua`). MAME loads part-way through a field and this
emulator between two, so for a file saved in the key wait three things are
left out: where in the wait loop each PC is, the per-field counters (FRAMES
and the key scan's `$3C27`) and the 32 bytes below SP, where each interrupt
leaves its pushes. MAME needs the disc ROM, which is in
`roms/JA-DOSROM/` and matches its hash, and the SP0256's, which is not to
hand: a zero-filled stand-in lets it start (`tools/mame/romset.sh`, all in
the ignored `out/`). Run 2026-10-04: the 102 files saved in the key wait
are the same in all of it; the 17 saved running differ by a few bytes and
registers, as a field's phase would make them, and are reported, not
judged; the 80 from 3K, 35K and 51K machines are not compared, MAME
refusing under 16K and reading nothing past `$8000` (`out/m11-mame.log`).

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
*Done when:* the board powers up to the Ace's prompt on the panel, a
blank screen with the cursor on the bottom line (the ROM prints `OK` only
after a line runs, as M3 found); `2 2 + .` typed **on
the PicoCalc keyboard** prints the answer; arrows, DELETE and BREAK work; a
`VLIST` scrolls cleanly with zero dropped snapshots over 3,000 fields.
*Measured:* core 0 share and host cycles per instruction at the prompt, in
compute and in scrolling (§14); longest present.
*Leaves out:* sound, the card, the menu.
*Done, 2026-10-04* (Plus 2 W, RP2350B rev 2, id `7458DC82A89AAC12`, at
150 MHz, gcc 15.2; measured 2026-10-03). The firmware is split as §4.1
says: `core0.c` runs the guest a field at a time against an absolute
deadline, `core1.c` presents and polls, and `handoff.c` holds the pool
under its spinlock. The board boots to the Ace's blank screen and cursor
on the panel. Typed **on the PicoCalc keyboard** by the owner, 2026-10-04:
`2 2 + .` prints `4  OK`; the arrows, DELETE and BREAK work; `VLIST`
scrolls cleanly, and the perf line's dropped count stayed 0. Over the UART:
`2 2 + .` read back `2 2 + . 4  OK` from screen RAM through
`tools/uart-screen.sh`; `VLIST` repeated ran over 4,000 fields with zero
dropped snapshots, at tier 0 and again at tier 2; an idle run of ~30,000
fields (10 minutes) had rt 1.000, no late fields, no drops, no I²C errors
and no lost log lines. **Measured:** core 0 at 20–39 % (§3.2's M7 table);
the longest present 11.86 ms, the boot's full redraw, and a scrolling
present ~11.4 ms. The shipping build (`PICO_ACE_UART=OFF`) was run by the
owner on a Pico 2 W, 2026-10-04, through the same keyboard checks, with
the same result and drops at 0; that build logs nothing, so its board id
was not recorded. *Not verified:* the field's line numbers against the
schematic; anything paced on audio (M8).

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
*Done, 2026-10-04* (Plus 2 W, RP2350B rev 2, id `7458DC82A89AAC12`, at
150 MHz, gcc 15.2). `beeper.*` and `audio.*` are pico-atom's, renamed; the
beeper takes the Z80's wrapping 32-bit T counter by difference, and
pico-atom's `test_audio` came with it, rebuilt on the real ROM's `BEEP`
(§8). `core0.c` drains each field's samples and pushes them, and the push
blocking on a full queue is the pacing; `PICO_ACE_AUDIO=OFF` keeps M7's
timer pacing. On the host: every half period of `BEEP` at m = 50, 100 and
300 is the hand count, 13m + 2 T; every sample matches an independent box
filter of the executed edges to 1 LSB; the pitch measured off the output is
the count's to 1 part in 10⁴; 3,004 fields make exactly ⌊T × 75 / 6,656⌋
samples, and so do 20 across the T counter's wrap. A stamp 11 T late and a
rate that drops the remainder each fail a check. **On the board:** a
10-minute run (`out/m8-soak.log`, 31,627 fields, with three `BEEP`s and a
`VLIST` typed over the UART) read 36,621 Hz consumed in 117 of its 5 s
windows and 36,620 in 8, after the first, which spans the start; **0
underrun samples, 0 late refills**, no core overflow, no late fields, no
dropped snapshots, no I²C errors and no lost log lines. The queue's low
water was 641 of 1,024. The board's speaker edges for `100 2000 BEEP`,
`300 2000 BEEP` and `50 1000 BEEP` were 5,000, 1,666 and 5,000, the host's
counts exactly. The prompt does not click (§8). The late path, which the
soak never took, was forced with a scratch build that masked core 0's
interrupts for 9 ms every 250 fields (`out/m8-late.log`): each stall counted
3 late refills and cost 3 halves of consumed samples, with no IRQ storm and
no underrun, and playback carried on. Core 0 with audio: §3.2's M8 table. The owner listened to `BEEP` on the PicoCalc's speaker,
2026-10-04, and it sounds correct. CI green on both jobs for PR #7. The
shipping build (`PICO_ACE_UART=OFF`) was run by the owner on a Pico 2 W,
2026-10-04, and its `BEEP` sounds correct; that build logs nothing, so its
board id and audio counters were not recorded. *Not verified:* the
shipping build's underrun and late-refill counts.

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
*Done, 2026-10-04* (Plus 2 W, RP2350B rev 2, id `7458DC82A89AAC12`, at
150 MHz, gcc 15.2). `sd.*`, `diskio.c`, `storage.*` and `fatfs/ffconf.h` are
pico-atom's, renamed; `sd.c`'s one `sleep_us` became `busy_wait_us_32`,
since it now runs on core 1 (HW §9.7), and card detect can be polled before
a card is first initialised. `settings.*` is pico-atom's parser and
rewriter with §10.6's keys, and `test_settings` passes with them; a planted
bug (a refused value counted as given) fails five of its checks.
`settingsio.*` is the read half of pico-atom's; the save is M10's. `card.*`
holds the jobs (mount, read, unmount, each timed) and the debounced slot.
`park.*` is pico-atom's park in its own file: core 0 parks between two
fields and feeds the queue silence, and on resume starts the held keys and
its measuring windows again. Core 1 checks on the park once a loop, so
between jobs it still presents, polls the keyboard and drains the log; a
job itself is synchronous and holds core 1 for its length. Every wait in
the SD driver is bounded, the longest being 1 s for a card that stays
`ACMD41` busy, inside the southbridge's 2.5 s watchdog (HW §6.1); the jobs
measured took 7–221 ms. A transfer that fails marks the drive
uninitialised, so the next mount runs `sd_init` again. M9's one
reason to park is the UART's hold (`tools/uart-hold.sh`), which runs the
card job on entry and again on each card change. Core 1's own log lines
wait for a line of core 0's that is half sent (`log_core1`).
**On the board:** with no card, the prompt appeared 478.5 ms after reset
(`out/m9-nocard.log`); with a card that has no `/ace/` (the owner's
pico-atom card), 681.5 ms, of which the mount was 191.8 ms and the search
for the file 7.6 ms (`out/m9-boot1.log`). With `/ace/pico-ace.cfg` holding
`ram = 3k` and `volume = 9` on line 4, freshly inserted: mount 211.3 ms,
the 231-byte file read and parsed in 9.7 ms, prompt at 702.5 ms; the log
and every heartbeat name `line 4: no such value`, and the machine is 3K:
`1 16384 C! 16384 C@ .` prints `255`, where the same card with the build
set to 19K prints `1` (`out/m9-cfg3k.log`, `out/m9-cfg3k-control2.log`).
After a reset that left the card powered, the mount took 14.6 ms and the
prompt came at 505.5 ms. Parked by the hold with the card in, the owner
pulled and reinserted it: card detect read out, in, out, in, the mount at
the first `in` failed with `FR_NOT_READY` and the one at the second took
191.8 ms; after 45.5 s parked the guest resumed and `2 2 + .` printed
`4  OK`, with 0 underrun samples and 0 late refills (`out/m9-hold.log`, HW
§7.1). That run was on a build before two fixes it prompted: core 1's log
line written into the middle of a heartbeat, and a heartbeat window across
the park counting its silence as consumed samples (369,224 Hz). With both
fixed, a 2.0 s park with no card read 36,621 Hz in the next window.
*Not verified:* an empty card (no spare card was to hand; the card without
`/ace/` takes the same path, `FR_NO_PATH`, to the defaults); a card that
does not mount for other reasons (exFAT, unformatted); a card pulled during
a job; the card swap while parked on the build with both fixes. The
shipping build (`PICO_ACE_UART=OFF`) was run by the owner on a Pico 2 W,
2026-10-04, and works; it logs nothing and has no hold, so its boot
timings, the settings file's problem line and a park were not observed on
it.

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
*Done, 2026-10-04* (Plus 2 W, RP2350B rev 2, id `7458DC82A89AAC12`, at
150 MHz, gcc 15.2). The trap is §10.3's, on the ROM's two block routines;
`test_tape` records the ROM's own SAVE off D3, plays it into the ROM's LOAD
and VERIFY, and the trapped calls (SAVE header and data, LOAD and VERIFY
header and data, a header asked for and data found) leave the same machine
as the ROM's routines in every byte of RAM and every register but R; the
same file serves `LOAD` end to end through the trap, and a damaged byte is
the ROM's error. Of three planted bugs one failed it; the other two changed
state the ROM's exit overwrites, and were dead. The menu, pause, Alt+R and
the settings save are §12's; volume, perf_line and boot_tape are applied.
**On the board:** `SAVE SQ` wrote `/ace/tapes/SQ.tap` (parks of 94.4 and
48.9 ms for its two blocks); after a reset `LOAD SQ` found it by name and
`5 SQ .` printed `25` (`out/m10-save-load.log`). With `SQ.tap` put in the
deck from the menu, `SAVE CUBE` appended to it, and after `FORGET SQ`,
`LOAD CUBE` read the tape in order, the ROM skipping SQ's header and the
deck SQ's data, and `3 CUBE .` printed `27` (`out/m10-deck.log`). The
owner power-cycled the PicoCalc and `CUBE` loaded and printed `27` again.
The card, read on the workstation, held both tapes block for block with
good checksums and no `.new` left. `LOAD SQSQ`, which no file answers, was
declined and the ROM waited on the tape input until BREAK. The archive's
`tut-tut.tap`, copied to the card, loaded in the 19K machine from the
deck and, after the header lookup was added, by name with the deck empty,
and `TUTTUT` ran to its title screen (`out/m10-tuttut.log`). The owner
checked Alt+M, F1 and Alt+P on the panel, and asked for the Ace's own
font and mixed case in the menu (§7.5).
*Measured:* a 16 KiB block (`BSAVE`/`BLOAD` of `$4000`, 19K) loads in a
41.9 ms park, 73 ms with its header, and saves in 94.4 ms; Tut-Tut's 11,998
bytes in 34.9 ms. Underrun samples 0 and late refills 0 through every load,
save, menu and pause, at 36,621 Hz consumed.
The owner saved settings from the menu on the Plus 2 W, and the tape in
the deck, the volume and the perf line came back after a reboot; the M9
test file's refused `volume = 9` on line 4 survived the save, because the
rewriter copied refused lines as they stood. It now writes the value in
force over a refused one, or comments it out if another line gives that
key a good value (§10.6, EL §8.7), with `test_settings` cases and a control that fails
under the old rule. On the board the owner's file, which by then also had
a good `volume` line appended by the old rule, was saved from the menu:
282 bytes became 284, line 4 a comment, and the next boot read it with no
problem (`out/m10-settings.log`). The owner ran
the shipping build (`PICO_ACE_UART=OFF`) on a Pico 2 W, 2026-10-04, and it
works (no UART, so no timings or board id).
*Not verified:* VERIFY and BVERIFY on the board (host only); a card pulled
during a tape job. Once during the
session the UART went silent with both cores later found in their normal
loops, and the Debug Probe stopped enumerating until replugged; it did not
recur after a reflash, and its cause is not known.

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
*Reference:* MAME, run headless (§13.4, §18 item 6).
*Done, 2026-10-04.* The format is
§10.5's, settled from the archive's description, MAME's loader and 199
files (§16). `snap_ace.c` imports, `snapshot.c` is pico-atom's `.sav` with
the Z80's fields, `sha1.c` is pico-atom's, and the Snapshot page is §12's.
**On the host** (Apple M1 Pro, Apple clang 21): `test_snap_ace` makes
files with an encoder written to the archive's description, from machines
the real ROM ran, and judges each by typing at the loaded machine: a word
defined before the save runs in the 3K, 19K and 51K, a file padded to
`$8001` loads in the 3K, a 19K file is refused by the 3K and the 51K naming
19K and leaves them untouched, a 35K file is refused by the 19K naming 51K
and loads in the 51K with `$04F7` written back; without that word the ROM
does not return to the prompt (the control). Files with no end mark, short
of `$4000`, too long, with a RAMTOP or IM no `.ace` has, or unreadable are
refused by the check with the machine untouched. All 199 archive files:
198 load (35 with the word written back), Ace Invaders is refused, and
every file saved in the key wait is still in it 100 fields on with its
screen. Against MAME, the 102 19K files saved in the key wait are the
same (§13.4, `out/m11-mame.log`). `test_snapshot`: a state saved 37 fields
into a program that scrolls and beeps, restored into a machine that has
been doing something else, meets the original 150 fields on in RAM, CPU,
T counter, speaker and budget, with the same speaker edges (a budget one T
out does not meet); 3K and 51K states round-trip and their words run; a
flipped bit, a short file, wrong magic, a newer version, an impossible
length, another ROM, field or RAM size are refused with the machine
untouched; the ROM's SHA-1 is §10.2's.
**On the board** (Plus 2 W, id `7458DC82A89AAC12`, 150 MHz, gcc 15.2):
`: sq dup * ;`, saved to slot 1 from the menu over the UART; after
`FORGET SQ`, `3 sq .` stopped at `sq`; slot 1 loaded, the saved screen came
back, and `3 sq .` printed `9  OK` (`out/m11-sav.log`). With the review's
fixes, `: cube dup dup * * ;` saved to slot 2, and after a reflash slot 2
loaded twice, the second time with the page remembering the slot, and
`2 cube .` printed `8  OK` (`out/m11-sav2.log`). Underrun samples 0 and
late refills 0 throughout, at 36,621 Hz consumed.
*Measured:* a 19K state (19,604 bytes) saves in 132.0 ms to a new slot
and 82.2 ms to another, and loads, both passes, in 47.0–47.1 ms on the
board.
The owner ran the shipping build (`PICO_ACE_UART=OFF`) on a Pico 2 W,
2026-10-04, with the staged set (`out/m11-card/`) on the card: in the 19K
machine Pacman and Othello, the set's 19K files, loaded from the Snapshot
page, and the rest, for 3K and 51K (35K) machines, were refused (no UART,
so no timings or board id). The menu cannot change the RAM size until the
Machine page (M15); `ram` in the settings file does (§10.6), and with the
machine booted as 3K and as 51K that way the owner loaded every other file
in the set (Golfgrid and Hangman in the 3K; Casse Briques, Moon Buggy and
Dreamsoft Racer, all 35K, in the 51K with the key wait's word written
back), and Ace Invaders was refused, as on the host.
*Not verified:* `.ace` load time on the device; a card pulled mid-save. After the first flash the
Debug Probe dropped off USB and needed a replug, as once in M10.

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
| Keyboard matrix | §2.4 | **ROM, executed** | **settled** 2026-10-03: every cell pressed at the prompt alone, with SHIFT, with SYMBOL SHIFT and with both (`test_keyboard`). The cells agree with MAME's table; the editing set did not agree with this design's earlier belief (up is SHIFT+6, down SHIFT+7, and SHIFT+3 types `3`) |
| Port read bits D5–D7 | tape on D5, rest high | schematic; ROM's tape loader | low-medium. MAME's `io_r` agrees: `$FF`, D5 cleared by the tape signal |
| `IN` vs `OUT` speaker direction | `IN` one way, `OUT` the other | schematic; ROM's `BEEP` | medium. MAME: `IN` low, `OUT` high, which `ace.c` follows. MAME also takes the tape output from **D3 of the `OUT`**, not from the access, which contradicts §2.3; settle in M13. The prompt makes no edge: its key scan is all `IN`s (`test_boot`). Executing `BEEP` (2026-10-04, §8) gives the manual's 8m µs only because each access moves the level: its `OUT` writes the counter's high byte, whose bits do not alternate. So the speaker follows the access; the polarity is still MAME's, and inaudible through the DC blocker |
| Display polarity | set bits white | ROM, executed, against photographs | high. **Executed** 2026-10-03: with set bits as ink, the character set the ROM writes reads as text on a paper ground that its spaces clear to (`test/host/golden/boot.ppm` and `glyphs.ppm`, looked at). That paper is black and ink white is from photographs |
| ROM uses IM 1 | yes | ROM | **settled** 2026-10-03: `IM 1` at `$008E`, `EI` at `$009F`; IM is 1 at the prompt in every machine (`test_boot`). The handler is at `$013A` |
| ROM halts when waiting for a key | no | ROM | **settled** 2026-10-03, by execution: at the prompt it spins on FLAGS (`$3C28`) bit 5 at `$059B`, which the interrupt sets on ENTER, and never halts (`test_boot`). It does `HALT` once a word in `VLIST` (`$0679`), found by the trace diff |
| RNG seed location | none | ROM | **settled** 2026-10-03: the ROM has no random-number word (its dictionary names were listed). The manual's `RND` keeps its own seed and seeds it from FRAMES (`$3C2B`), which the interrupt counts, so zeroed RAM leaves nothing stuck (§6.3) |
| Key minimum hold, in fields | 3 scans | **ROM, executed** | **settled** 2026-10-03: the interrupt's scan (`$0310`) counts a held key down from `$20` in `$3C27` and takes it on the third consecutive field; the next key needs one field with every key up; a key held 33 fields repeats, and then every 4. Typing a line at 2 fields held or with no gap loses keys (`test_keyboard`'s controls). The replay uses 4 and 2 (§9.1) |
| `.tap` block layout | §10.3 | ROM tape routines; archive files | **settled** 2026-10-04: on tape a block is the flag byte (`$00` header, `$FF` data), the bytes, and their XOR; the `.tap` keeps a 2-byte little-endian length (bytes + 1), the bytes and the XOR, with no flag, and the ROM writes a 25-byte header then its data. The archive's `tut-tut.tap` (jupiter-ace.co.uk, fetched 2026-10-04) is exactly that: a 26-byte block naming `TUTTUT`, then 11,998 bytes, both XORing to zero. A block's flag is therefore taken from its place, even blocks headers (`tape.h`) |
| `.ace` snapshot encoding | §10.5 | the archive's FAQ; MAME's loader; sample files | **settled** 2026-10-04 from the Jupiter Ace Archive's FAQ, MAME's `snapshot_cb` and 199 files (the archive's 4, TOSEC's 195): RLE with `ED`, RAMTOP at `$2080`, registers from `$2100` in 32-bit words with noise above the value, no dump past `$7FFF`. The key wait's stack is `$04F7` at RAMTOP − 2 in all 135 files that hold it. `test_snap_ace` loads all 199 (198 load, 1 refused for its stack); MAME agrees on the 102 19K files saved in the key wait (§13.4) |
| Tape signal timings | — | ROM tape routines | unknown |
| Tape block routines | load `$18A7`, save `$1820` | ROM | **settled** 2026-10-04 by reading them (`tape.c` names every address used) and by execution: `test_tape` records the ROM's own SAVE off D3, plays it into the ROM's LOAD and VERIFY, and the trapped calls leave the same machine in every byte of RAM and every register but R. The tape input is D5; the loader keeps the last level it saw in C. The signal must be played inverted against D3 for the line to rest at the input's idle level (D5 high) between blocks |

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
   *Chosen 2026-10-04:* **MAME, run headless**, which changes "MAME is read
   here, not run" for this check only (§13.4).
7. **A 35K `.ace` loads into the 51K machine.** *Decided 2026-10-04.* There
   is no 35K machine (§6.2), and 36 of the archive's 199 files were saved on
   one (§10.5). They load into the 51K with their bytes as they are and only
   the key wait's missing stack word, `$04F7` at `$BFFE`, written back,
   rather than being moved down into the 19K or refused.

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
