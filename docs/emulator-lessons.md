# Writing an emulator for the PicoCalc: what we learned

A standalone guide for developers and agents starting an emulator of a vintage
machine on the **ClockworkPi PicoCalc**, in C against the Raspberry Pi Pico SDK.
It collects what two complete emulators taught us about architecture,
accuracy, performance, testing and process:

- an **Acorn Atom**: a 1 MHz 6502, an MC6847 video display generator, an
  8255 PPI, a 6522 VIA, an 8271 floppy controller, a 1-bit speaker,
  cassette and disc;
- a **Jupiter Ace**: a 3.25 MHz Z80 with a Forth ROM, a 32×24 character
  display built from 768 bytes of screen RAM and 1 KiB of write-only
  character RAM, a 40-key matrix, and one port bit each for the speaker
  and the tape.

The Ace was built second, from this guide and by reusing the Atom's drivers
and much of its core (§14.6), and what it added is folded in. None of it
depends on either project's source. Copy it into a new project as is.

**Companion document.** `hardware-notes.md` is the authority on the host: the
PicoCalc's wiring, peripheral protocols, timing and measured costs.
References written **HW §N** point there. Plain **§N** points within this
document. This guide does not repeat the hardware notes. It says what an
emulator does with them.

**Scope, October 2026.** The figures are measurements from a Pimoroni Pico
Plus 2 W (RP2350B) at 150 MHz unless stated otherwise. Each is the number for
one guest on one board, and is here to calibrate your estimates, not to
replace your own measurements. Each example names its machine (the Atom or
the Ace) and the guest part involved (a 6502, an MC6847, an 8255, a 6522, an
8271, a Z80), so the lesson can be mapped onto the parts in yours.

**Contents**

1. [The shape of the problem](#1-the-shape-of-the-problem) ·
2. [Architecture](#2-architecture) ·
3. [The CPU](#3-the-cpu) ·
4. [Devices and the bus](#4-devices-and-the-bus) ·
5. [Video](#5-video) ·
6. [Audio](#6-audio) ·
7. [Keyboard](#7-keyboard) ·
8. [Media: ROMs, tape, disc, snapshots, settings](#8-media-roms-tape-disc-snapshots-settings) ·
9. [Timing and clocks](#9-timing-and-clocks) ·
10. [The user interface](#10-the-user-interface) ·
11. [Testing](#11-testing) ·
12. [Measuring](#12-measuring) ·
13. [Working on the hardware](#13-working-on-the-hardware) ·
14. [Process and documentation](#14-process-and-documentation) ·
15. [Checklist for a new emulator](#15-checklist-for-a-new-emulator)

---

## 1. The shape of the problem

**The SPI wire to the LCD is the bottleneck, not the guest CPU.** A 1 MHz
6502 interpreted on a 150 MHz Cortex-M33 takes 35–46 % of one core, and a
3.25 MHz Z80, once tuned, at most 23 %. A full redraw of a 256×192 guest screen
takes 11.5 ms of a 16.7 ms field, all of it wire time (HW §4.7). Put
optimisation effort on pixels transmitted, not on the interpreter, until a
measurement says otherwise.

**SRAM is the scarce resource**, not CPU (HW §2.3), at least for a machine
with a large address space and disc. The whole Atom, with a 64 KiB address
space, three video snapshots, a 64 KiB tape buffer, the renderer and 25 KiB
of interpreter moved into SRAM, came to ~255 KiB, 49 % of an RP2350's
520 KiB. On an RP2040 the same image would leave ~5 KiB. The Ace, with
60 KiB of guest memory, the same tape buffer and 33 KiB of interpreter in
SRAM, came to ~215 KiB, 41 %, and memory never constrained it. Decide early
whether you support RP2040; for the Atom it was a memory question first.
The Ace dropped it as a CPU question, on an estimate of the Z80's cost that
the measurement later halved, and the question was not reopened.

**Do not trust an estimate of interpreter cost, in either direction.** The
Atom's design estimated 40–80 host cycles per 6502 instruction. The
measurement was 158–218: about 100 Thumb instructions per guest instruction
once the run loop, device ticks, interrupt lines, dispatch and the opcode
are counted. That left roughly 2× headroom at 1 MHz, not 6–12×. The Ace's
design then scaled that measurement up for the Z80's prefixes, 16-bit
operations and extra flags, to 180–280, and the Z80 alone on the board
measured 82–103: a third to a half of the estimate. The whole Ace machine
then cost up to twice the CPU-alone figure (a compute loop at 160 host
cycles per instruction from flash, against the bench's 82), for its bus,
interrupt, the real ROM's instruction mix and core 1 sharing the XIP
cache. So **measure the CPU alone on the board before any other
port work** (§14.3's gate), and the whole machine as soon as it boots.
Measure before you promise turbo modes or faster guest clocks (§9.3).

**Pick the clock you ship at by the SPI divider, not by the CPU.** Only
150 MHz and 300 MHz deliver the panel's full 75 MHz SPI rate (HW §3). At
200 MHz the guest runs 1.33× faster and the display 1.5× slower. Ship at
150 and treat 300 as a user's opt-in overclock (§9.4). The Ace never needed
more than 150 MHz, and left the 300 MHz path unbuilt.

---

## 2. Architecture

### 2.1 A portable core and a thin port

Split the code in two:

| Layer | Contains | Rules |
|---|---|---|
| **core** | CPU, bus, every guest chip, media formats, keymap data, snapshot format, settings parser, status text | portable C, **no SDK header, no dynamic allocation**, all state in one machine struct or statically sized buffers |
| **port** | LCD, audio, I²C, SD, the two cores' loops, menus, file I/O | the only place SDK headers appear |

Build the core with the workstation's compiler under CTest, and the firmware
with `arm-none-eabi-gcc`, **both under `-Wall -Wextra -Werror`**, in CI on
every push. The core building clean on both toolchains is the mechanism that
stops SDK dependencies leaking down. A rule in a document does not do that;
a failing build does. Prove it once: put an SDK `#include` in a core header
and watch the host build fail. It also means the CPU passes its test suites,
and the whole machine boots its real ROMs, on a laptop before any hardware
exists.

Put **every fixed capacity in one header** (address space, buffer sizes,
list lengths, queue depths). You will trade them against each other
repeatedly, and the SRAM budget is only a link-time fact if they are in one
place. Check growth with `arm-none-eabi-size` on every build; have CI print it.

**Split the port's big files from the start.** The Atom's `main.c` (core 1's
loop, parking, boot media, UART keys, measurement) and its menu each grew to
about 1,200 lines, the two largest files in the tree. The Ace gave core 0's
loop, core 1's loop, the park, the frame handoff and each menu concern a
file of its own from the first day.

### 2.2 A narrow, synchronous core API

The seam between core and port should be a handful of calls:

```c
void     machine_init(machine_t *m, const machine_config_t *cfg);
void     machine_reset(machine_t *m);
uint32_t machine_run(machine_t *m, uint32_t cycles);  /* returns cycles actually run */
uint32_t machine_run_field(machine_t *m);             /* one video field, split at flyback */
void     machine_key_set(machine_t *m, int row, int col, bool down);
size_t   machine_audio_drain(machine_t *m, int16_t *dst, size_t max);
const uint8_t *machine_vram(const machine_t *m);
uint8_t  machine_video_mode(const machine_t *m);
void     machine_copy(machine_t *dst, const machine_t *src);
```

Every test exercises this seam, and so does the firmware. When the field loop
lives in the core (`run_field`), the host tests run the exact code the
firmware runs. The video calls return **every input the image depends on**:
the Ace has no mode byte, but its seam returns the character RAM beside the
screen, because a program can redefine any glyph (§5.3).

**`run(n)` executes whole instructions until at least `n` cycles have
elapsed and returns the true count.** The caller carries the overshoot as a
debt into the next call. Never add a "run exactly N cycles" variant. The
debt-carry pattern is what keeps long-run timing from drifting, and it
survives splitting a field into several runs (§5.6), because each call
reports its true count against one accumulator.

**Copy a machine with a function, never with `=`**, if the struct holds
pointers into itself (a page table pointing into its own RAM). A struct
assignment leaves the copy reading and writing the original's memory; the
copy function rebuilds the page table against the copy. The same goes for
host callbacks stored in the machine: a copy must not inherit the original's
hook, or the copy will drive the original's pins.

### 2.3 Which core owns what

| Core 0 | Core 1 |
|---|---|
| guest CPU and every guest chip | LCD presenting |
| audio synthesis, and the audio DMA IRQ | southbridge I²C (keyboard, battery, backlight) |
| pacing (§6.3) | SD card work, menus |

Core 1 owns every slow peripheral, so core 0's only blocking point is the
audio queue it paces on. Two invariants:

- **Core 1 never reads guest RAM while the guest runs.** Everything it draws
  arrives in an immutable snapshot (§2.4).
- **Core 1 never calls `sleep_us`/`sleep_ms` in its loop.** Those set an alarm
  whose IRQ runs on core 0, in the middle of the guest (HW §9.7). Wait with
  `busy_wait_us_32`. Fixing this one call was worth 1.11× on the Atom's
  guest, more than moving the interpreter into SRAM, and it cut run-to-run
  spread from ±4 to ±0.1 cycles per instruction. **Check code you move onto
  core 1 too**: the SD driver the Ace took from the Atom had one `sleep_us`
  of its own.

**Core 0 never calls `printf` once the guest runs.** At 115200 baud a
few-hundred-byte heartbeat takes ~30 ms, and the audio queue's low-water slack
was ~11 ms. Format log lines into a ring on core 0 and let core 1 move bytes
into the UART FIFO as it has room. Count dropped lines. If core 1 logs too,
its lines must wait for a line of core 0's that is half sent; the Ace's
first version wrote core 1's line into the middle of a heartbeat.

**Keep renderer state off core 0.** The video renderer's lookup tables belong to
the presenting core. If core 0 rebuilt a LUT on a guest register write while
core 1 expanded rows through it, that is a cross-core race no care in the
renderer can see, and a second copy costs SRAM you do not have. Core 0 carries
VRAM and the register latches; the mode byte is read off them when the
snapshot is taken.

### 2.4 The frame handoff: three buffers, four states

Core 0 publishes a snapshot of VRAM plus mode bits (plus a few status bytes)
at each field's end. Core 1 presents the newest. Use a pool of **three**
buffers, each `free`, `filling`, `ready` or `rendering`:

- Core 0 marks its `filling` buffer `ready` and claims a `free` one.
- **Publishing drops any older `ready` buffer on the spot.** At most one is
  ever `ready`, so core 1 needs no sequence numbers, and core 0's next claim
  always finds a `free` buffer.
- Core 1 takes the `ready` one, marks it `rendering`, frees it when done.

Why three: core 1's worst case can exceed a field. With two buffers core 0
would have to block (and corrupt the schedule audio is paced against) or
rewrite a buffer core 1 might be claiming. The third buffer deletes the
class of problem. Do not economise here.

Write the state machine in the core with **no lock of its own**; the port
calls each transition under one SIO spinlock (RP2350 has no compare-and-swap,
and masking interrupts locally is not a multicore lock). A lock-free state
machine can be driven through a long randomised interleaving in a host test
(the Ace's runs 100,000 transitions: core 0 always gets a buffer, never the
one core 1 holds, at most one is ready, and every publish is taken or
counted dropped). Count dropped snapshots; that counter is how you learn
core 1 is saturated. Simulation timing is never sacrificed to presentation.

The pool moved from the Atom to the Ace with only the snapshot's contents
changed. A snapshot of 768 screen bytes, 1,024 character bytes and a few
status bytes costs core 0 a few microseconds to copy.

### 2.5 Parking the guest for host work

Anything slow on core 1 that touches the machine (an SD read for a tape or
disc request, the menu, a pause, a restart) uses one mechanism: **core 0
parks at a field boundary and hands the machine to core 1**, feeding the
audio queue silence at the rate it drains so audio neither underruns nor
loses its pacing. Core 1 does its work with the card mounted, then hands the
machine back. Guest time does not advance while parked, so card latency is
invisible to the guest, and pacing needs no catch-up on resume.

Core 1 may write the machine (load ROMs, re-run `init`, load a snapshot)
only inside that window. Settings that change on core 1 (a keyboard layout
chosen by a tape load) are applied by core 0 when it takes the machine
back, so there is never a second writer.

Core 1 checks for a park request once per loop, so between jobs it still
presents, polls the keyboard and drains the log; a card job itself is
synchronous and holds core 1 for its length. **On resume, restart every
measuring window** as well as the held keys. An Ace heartbeat window that
spanned a park counted the silence it fed as consumed samples and reported
369,224 Hz.

---

## 3. The CPU

### 3.1 What accuracy to aim for

**Cycle-correct at instruction granularity** is cheap and is what makes timed
loops, tape loaders and raster tricks work: every documented opcode's cycle
count, page-crossing and branch penalties, exact decimal mode including the
NMOS flags, and the quirks real software depends on (`JMP (xxFF)`, B flag,
read-modify-write double write). Sub-instruction (per-cycle bus) accuracy was
not needed for anything either machine's software archive does; drop it
unless a known title needs it. Bus contention can still be modelled at
instruction granularity (§4.5).

**On a 6502, trap and count undocumented opcodes** in a first version, and
put the count on the heartbeat. Zero over a soak is evidence; a nonzero count
tells you which title needs them.

**On a Z80, implement the undocumented behaviour instead**: the IXH/IXL/IYH/IYL
forms, `SLL`, the `DDCB`/`FDCB` register-copy forms, flag bits 3 and 5,
`MEMPTR` and its leak into `BIT n,(HL)`, `R` per M1 cycle, the `LD A,I`
P/V quirk. It is thoroughly documented, the standard test suites check it,
and Spectrum-era habits mean software uses it. Count the `ED` holes (the
opcodes that act as two-instruction NOPs) on the heartbeat instead. Model
**Q**, the shadow of F that `SCF` and `CCF` take X and Y from (`(Q ^ F) | A`,
from Patrik Rak's measurements of NMOS parts): FUSE's tests cannot see it,
since each starts with Q at zero, so it needs a test of your own (§3.3).

**Where the references disagree, choose one authority** (the suite you are
checked against) and write down each choice it forces. For the Ace's Z80,
following FUSE meant four: repeating block instructions keep the single
instruction's flags; a `JR cc` or `DJNZ` not taken does not read its
displacement; `HALT` leaves PC on itself and repeats as a NOP; IM 0 executes
only an `RST` from the bus. Each was checked against the guest's bus, where
none can be seen.

### 3.2 Implementation

A **`switch`-dispatched interpreter** with explicit cycle accounting. On a
Cortex-M33 a dense switch compiles to a table branch, keeps the CPU state in
registers, and beats a table of function pointers. The CPU has no notion of a
device; the bus does (§4). For a Z80, one `switch` per prefix page, with
`DD` and `FD` sharing one body parametrised by the index register. The Ace
left the CPU state in its struct, reached through a pointer; copying it into
locals across the dispatch loop was planned as an experiment and never
needed (§3.4).

Interrupt lines as a **bitmask of asserted sources** (level-sensitive IRQ:
zero deasserts) and NMI as **edge-triggered and latched**. A reset line that
resets the CPU must also reset every chip on the real reset line. A timer
interrupt left enabled across a reset ran garbage on the Atom until the VIA
was reset with the CPU. That the VIA is on the reset line was inferred from
what the ROM's reset routine assumes, not read off the schematic; say so
when you do the same.

**ROM traps** (§8.2) cost the run loop something however they are done. The
Ace's run loop has a second copy, used only while any trap is set, that
looks the PC's low byte up in a 256-entry table before each instruction:
one load, measured at 0.7–0.9 points of core 0.

### 3.3 Test suites, and what a skip means

For a 6502:

- **Klaus Dormann's 6502 functional test** must run to completion. It is the
  difference between an emulator and a plausible one.
- **Bruce Clark's decimal test** with **every flag checked**. Dormann's copy
  checks only A and C; Clark's catches a decimal `ADC` that takes N from the
  result rather than the intermediate. It is public domain, so its source can
  live in the tree; assemble it at fetch time (cc65's `ca65`/`ld65`).
- **The cycle table asserted by execution**: run each opcode in each
  addressing mode and count, rather than comparing two tables in the same
  repository. Only the first catches an addressing-mode bug.

For a Z80:

- **ZEXDOC and ZEXALL** (Frank Cringle), under a minimal CP/M BDOS stub: a
  `CALL 5` handling functions 2 and 9 is all they use. ZEXALL runs about
  47 G T-states, 90 s in a Debug host build and 26 s in Release. Register it
  under a label (`long`) so the quick run can leave it out.
- **FUSE's Z80 tests**: per opcode, every register, `MEMPTR`, the T-states,
  memory, and the order of memory and port accesses with their addresses and
  data. That is the cycle table asserted by execution.
- **Behaviour tests of your own** for what neither suite sees: `EI`'s delay,
  `HALT` and `R`, IM 0/1/2, INT held against pulsed, `LD A,I`'s P/V, NMI and
  `RETN`, Q, the run contract. Each rule beside a control that must fail.
- **Show the harness fails.** In the Ace's tree, planted one at a time:
  `DJNZ` taken at 12 T, `LD A,(nn)` leaving `MEMPTR` at `nn`, `EX (SP),HL`
  writing its bytes in the other order, and `BIT n,(HL)` taking X and Y from
  the operand each failed FUSE. `SCF` without Q passed FUSE and failed the
  behaviour test, which is why that test exists.

Do not commit test binaries you did not build; fetch them with a script into a
gitignored directory the test finds without configuration. A missing binary
makes the test report **skipped** (CTest exit code 77), and **a skipped
functional test is an unverified CPU**, not a pass. CI fetches the suites on
every run.

### 3.4 Making it faster, if you must

Measure first (§12). Then, in order of what paid off:

1. **Remove interference.** The `sleep_us`-on-core-1 alarm (§2.3) was worth
   more than any code placement.
2. **Make device ticks countdowns** (§4.3). A VIA tick rewritten as "subtract
   from T1 and from one counter to the next T2/shift-register event, call
   out of line only when one runs out" became nine instructions with no stack
   frame, and the whole machine got 3.6–6.2 % faster than before the VIA was
   completed.
3. **Move hot code to SRAM in measured tiers** (HW §9.2, §9.8). For a 6502
   interpreter this gave 1.12–1.19× for 25 KiB, a third of what it gave a
   tree-walking interpreter: `-O3` inlines the bus into every opcode and makes
   the dispatch 20 KiB, but the opcodes a real program runs fit the 16 KiB XIP
   cache anyway. Move small hot callees first (bus slow path, device ticks,
   ADC/SBC), then the dispatch loop. A 256-byte const table moved to SRAM
   measured nothing. Keep tier membership in one header of macros so a tier
   is a build option, and build each tier in its own build directory. For
   the Z80 the small callees gained about 1 point of core 0 on their own;
   the whole interpreter (33 KiB) took a compute loop from 31.8 % to 23.3 %
   and made results repeatable to 0.1 point (§12); and the 8 KiB ROM copied
   into SRAM on top gained nothing, since with the interpreter gone from the
   XIP cache the ROM fits it.
4. **Skip what the guest is only waiting through.** A `HALT` repeated until
   the next interrupt is free to skip if the interrupt line changes only
   between run slices: add the repeats' cycles and refresh counts at once.
   On a Z80 Forth whose screen listing halts once a word, that took the
   listing from 42 % of core 0 to 3 %. Hold a skipping machine to a
   stepping one, register for register, on the real ROM. Find out first
   whether the ROM halts where you hope: the Ace's prompt spins on a flag
   the interrupt sets, so the skip did nothing for idle.
5. **Keep the run loop bare.** Put a skip in the opcode that causes it,
   not in a check before every instruction. On the Z80 such a check cost
   10 host cycles an instruction by itself, and it also stopped GCC inlining
   the step function into the loop, which cost 20 more: together 23 % on a
   compute loop, found only because the same code was measured against
   the previous release in one sitting. Look at the loop's disassembly
   after any change to it, and mark the step `always_inline`.
6. **Stop when no result could change a decision.** Computed `goto` dispatch
   and CPU state in locals were never tried on the Z80: with the heaviest
   workload at 23 % of core 0, neither could. Leave them written down as
   available.

---

## 4. Devices and the bus

### 4.1 A page table with a NULL slow path

Back the whole address space with a flat array and dispatch every access
through a 256-entry page table:

```c
typedef struct { uint8_t *read; uint8_t *write; } page_t;  /* NULL -> slow path */
```

RAM and ROM are one indexed load or store (`write` NULL for ROM). I/O and
unpopulated pages take the slow path. Keep the descriptor **exactly two
pointers**; put per-page flags (ROM, I/O, VRAM, open bus) in a separate byte
array, because only the slow path reads them and a third field pads the
descriptor and grows the table.

**Mirrors cost nothing**: point several pages at the same RAM. The Ace's
1 KiB blocks each repeat across 4 KiB, and its table does all of it.
**Write-only memory** (the Ace's character RAM, which only the video
circuit reads) is a page with a `write` pointer and a NULL `read`.

**A device smaller than a page, inside RAM, is silently invisible.** The
Atom's floppy controller sits at eight bytes inside an otherwise-RAM page.
With that page's `write` non-NULL, the controller is never reached and the
disc system simply does not respond. Mark such a page I/O and split it in
the slow path. One page losing its fast path cost nothing measurable.

**Decode I/O by mask, not equality**, where the original decodes partially:
software relies on mirrors. The Ace decodes its one port on A0 alone, so
every even port is it. **Unpopulated memory reads open bus**, the last
value on the data bus, not `0xFF`. A ROM may probe an empty socket and act on
what it reads; a reference emulator that returns 0 there will diverge from
you in a trace (§11.4). Read the schematic for what open bus is on your
guest: the Ace's data-bus pull-ups are marked "not installed", and an
undriven read sees the video circuit's current fetch through resistor
arrays. Neither reference emulator models that. The Ace's ROM sizes RAM by
writing a value and reading it back, so any other value works, and the read
was left as configuration with the finding recorded.

**VRAM needs no write hook** if the presenter diffs snapshots (§5.3). That is
cheaper than tracking stores and cannot under-mark.

### 4.2 Model the chip, then wire it

Write each chip as the generic part (an 8255, a 6522, an 8271) with the
machine's wiring as accessors on top. Then the chip's own datasheet is the
test plan, and the wiring is a small, separately checkable table.

Things that a plausible model gets wrong:

- **Output latches separate from input sources.** A port read returns input
  pins alongside the last value latched into output bits. Merge them and a
  read-modify-write in the ROM corrupts outputs.
- **Bit-set/reset control paths** exist and ROMs use them (the 8255's BSR is
  how the Atom toggled the speaker).
- **One register, two meanings.** If a port carries both the keyboard column
  and the video mode, every keyboard scan writes the video mode too. Compare
  the relevant bits, not the whole byte.
- **An "optional" chip may be load-bearing.** The Atom's ROM read the
  optional VIA on every character it printed and hung before its first prompt
  without one. Fit what the ROM touches.
- **Every access the hardware decodes has its side effect.** On the Ace an
  `IN` from the port drives the speaker one way and an `OUT` the other,
  including the `IN`s that only read the keyboard. Do not filter out
  "keyboard-only" reads.
- **Check which bus cycles count as an access.** A Z80 interrupt acknowledge
  asserts IORQ without RD, so on the Ace it does not read the port and must
  not move the speaker. The Ace's bus test plants an acknowledge that reads
  the port, and fails.

**Let evidence decide how much of a chip to build.** Before completing the
6522's shift register and handshake lines we searched the Atom's whole
software archive (5,610 files) for code that enabled them: none did. We
completed the part anyway so nothing would meet a dead register, but the
search told us it was not urgent and which modes the games actually used.

### 4.3 Bring devices up to date lazily

**Nothing is clocked per instruction unless it must be.** The pattern that
made device after device free:

- **Bring an input up to date when the guest reads it**, the only time it can
  be seen. A cassette waveform, a timer's count, a 2.4 kHz reference: compute
  where it is now from the cycle count.
- **Keep "the first cycle at which anything here can next change".** A read
  before it is one subtract and a branch. The Atom's tape port's first
  version made three calls per port read and cost 18 % on a scrolling
  workload; the next-event version cost 3.5 %.
- **Stop the run slice at a device's next event** instead of checking per
  instruction. The floppy controller cost one compare per slice and nothing
  measurable overall.
- Bring everything up to date at the field boundary too, for the menu,
  snapshots and anything core 1 shows.

When a device's internal counters lag between events, give it an explicit
`sync()` and call it before anything outside the device reads them.

**Make the cycle count the only clock.** Every device times itself in guest
cycles, never in wall time. Then turbo is free (§9.3), pause is free, a
snapshot captures time exactly, and a host-side stall (card I/O) is invisible.

A machine with no timer chip makes this easy. The Ace's run loop checks only
the slice end and the INT line per instruction; everything else (the tape
signal, the speaker, wait states) is brought up to date at an access.

### 4.4 Hooks to the outside world

A device that reaches real pins (a user port on spare GPIOs) gets a callback
in the machine struct, NULL unless the port installs one. The bus calls it
only on the registers that matter (before a read of the input register;
after a write to output, direction or control). Test that the hook fires for
exactly those registers and no others. Output that changes without a
register write (a timer driving a pin) is pushed once a field.

### 4.5 Contention and wait states, at instruction granularity

Some guests stall the CPU while the video circuit fetches. On the Ace, a
memory access to one mirror of screen or character RAM during active display
is held until the line's visible part ends; the other mirror lets the CPU in
at once and shows snow on the picture.

- **Read the circuit from the schematic.** Neither reference emulator for the
  Ace modelled the waits, so there was nothing to diff against. The schematic
  showed the Z80's clock is the pixel counter's first stage, so the CPU is in
  step with the beam and a hold is a fixed function of the cycle within the
  field: an access at T h < 128 of a display line waits 128 − h.
- **Place each access at its instruction's start.** An interpreter exact per
  instruction has no time within one. Moving the Ace's access 4, 8 or 11 T
  later shifted a screenful's timing by 12 T in 625,334, because after the
  first hold the CPU is in step with the end of the window and only the first
  access of each burst depends on where it falls.
- **Put only the pages that can wait on the slow path.** The model cost the
  Ace at most half a point of core 0.
- **Measure the effect before deciding.** On the Ace the ROM printing a
  screenful took 15.9 % longer, a game ran 3.8 % fewer instructions a field,
  and a print loop was held 29.5 % of the time. That is enough for a program
  timed by its own drawing to see, so it was modelled and turned on.
- **Keep it switchable**: a build option for the control, off in the
  trace-diff harness when the reference lacks it, and recorded in save
  states (§8.5). Put the held share on the perf line.

---

## 5. Video

### 5.1 Do you need a framebuffer?

If the guest's image is a **pure function of its VRAM plus a few mode bits**
(true of most 8-bit video chips without sprites or mid-frame tricks), keep
no decoded framebuffer. Snapshot VRAM, and generate rows from the snapshot
straight into two RGB565 DMA line buffers (HW §4.6, §4.10). That saves
memory, but the decisive argument is correctness: there is no second copy of
the screen to keep in sync. A guest with sprites, raster effects or a
per-line palette needs a per-line state record in the snapshot, or a real
framebuffer; decide which by what its software does. A guest whose
character set is in RAM, like the Ace, still needs none: the image is a
function of 768 screen bytes and 1,024 character bytes.

### 5.2 Rows from a lookup table

- **One LUT per mode**, mapping a VRAM byte straight to its already
  horizontally stretched run of RGB565 pixels. The Atom's was 8 KiB worst
  case and rebuilt only on a mode or colour-set change, a few times per
  program run. A monochrome character display needs one 256 × 16-byte LUT
  (4 KiB) from a glyph row's bits to eight pixels.
- **Copy runs with unrolled code, not `memcpy`.** At 16–32 bytes the call is
  the cost (HW §9.4).
- **Vertical stretch is free.** A row repeated ×2 or ×3 re-sends the same line
  buffer to the next window rows without regenerating it.
- **Text modes take their own generator**: per cell, a glyph row from the
  character ROM or a synthesised block-graphics pattern, through a two-colour
  path. An inverse bit is an XOR of the glyph row.
- **The palette is a pointer.** A colour/mono switch rebuilds the LUT once and
  costs nothing per pixel. Derive a mono palette from the video chip's
  datasheet luminance levels, not from a greyscale formula over RGB.

Measured: a full 256×192 redraw cost the same as filling the same rectangle
with a constant colour, to within 0.4 %, in every mode. **Row generation is
hidden behind the DMA**; the present is wire-bound.

### 5.3 Dirty bands, and why the mode belongs in the shadow

Divide the guest screen into **bands of 8 rows**, each with an inclusive
`[min..max]` column span. Per presented snapshot:

1. **Compare the snapshot's mode byte with the presented mode.** If it
   differs, mark every band dirty and rebuild the LUT.
2. Otherwise diff the snapshot against a shadow copy, band by band.
3. Send each dirty band's span as one LCD window.
4. Copy the snapshot into the shadow **and its mode byte into the presented
   mode**.

Step 1 is a correctness requirement: a mode or colour-set change repaints
every pixel while leaving VRAM byte-identical, so a VRAM-only diff finds
nothing to do and leaves the old mode on screen for ever.

**Everything else the image depends on belongs in the shadow too.** On the
Ace, a program that redefines a character in RAM changes every cell showing
it while the screen bytes stay identical, and character-set animation is a
common technique. Diff the character set against its shadow into a
"changed glyph" mask (128 bits for 128 glyphs), and mark a cell dirty if its
screen byte changed **or** its glyph is in the mask. Test it with a simulated
panel brought up to date from the bands alone over a few hundred random
edits, a third of them to the character set only, matched against a full
render after each; run the screen-only diff as the control, which must
leave stale pixels.

Over-mark rather than under-mark: an extra band costs microseconds, a missed
one leaves a stale image for ever. Expect a fixed cost per present (the diff
and shadow copy, ~0.1 ms for 6 KiB) and per band (a window and its row DMAs):
one changed cell cost 0.28 ms, not the 0.05 ms the pixel count predicts.
Measured scrolling text: 0 dropped snapshots over 3,300 fields; presents of
0.1–5.5 ms.

### 5.4 Geometry and what to leave out

Draw the guest **1:1 at its native resolution**, centred, if it fits the
320×320 panel. A 256×192 guest at (32, 64) leaves a 64-row band above and
below. Scaling to 320×240 puts 56 % more pixels on the wire for a 1.25×
stretch with visibly uneven pixel doubling; we planned it and dropped it,
twice.

Use the spare bands for a **status line** (tape position, disc activity) and
a **perf line**, each drawn **only when its text changes**, and each hidden
by a setting. A 40-column line is 3,840 pixels, under 1 ms. Draw a guest
border only when its colour changes; never as part of every full redraw.

**Tearing**: there is no TE line (HW §1.2, §4.9). Accept it. Small, localised
band presents confine a tear to one band. Hardware vertical scroll (HW §4.8)
was not worth the y-remap hazard for a guest that scrolls by memory moves the
diff already catches cheaply.

### 5.5 Character ROMs

- **Never fabricate a font.** Take a verified extracted table (with its
  licence and attribution kept), render the whole glyph set to an image, and
  read it by eye.
- **Assert the layout against the data**: which bits hold the glyph, which
  rows of the cell, and the chip's glyph order (often not ASCII). Then a
  differently laid-out substitute fails loudly instead of rendering
  plausible-but-wrong glyphs. The Atom's first rendering put every character
  against the left edge of its cell because of an unneeded shift.
- **A screen full of glyph 0 at power-on can be correct.** Zeroed VRAM shows
  glyph 0 everywhere until the ROM clears the screen. Do not make the
  renderer substitute blanks; that would hide a ROM that never ran. Pin the
  behaviour with a test. Where the ROM writes a RAM character set at reset,
  as the Ace's does, zeroed character RAM draws all paper until it does;
  the Ace's ROM had written the set by the end of its first field.
- If the font is missing, draw a visible placeholder per cell and say so at
  boot, rather than a blank screen.

### 5.6 Split the field at flyback, and snapshot where the next frame begins

The guest polls a vertical-sync flag, or takes a field interrupt, to time its
screen writes. Three rules:

- **Guest instructions must run while the flag is in its flyback state.**
  Running a whole field and then pulsing the flag leaves a program that polls
  it spinning for ever. Split the field into runs at the flag's edges:
  active lines, flag low, remaining blank. Debt carry (§2.2) keeps the split
  exact.
- **An interrupt is a level with a duration, not a point.** A Z80 samples INT
  at the end of each instruction; a guest that keeps interrupts disabled
  longer than INT is held loses that field's interrupt, on the real machine
  too. Stop a run at both of INT's edges. Before the schematic settled the
  Ace's INT length, executing the ROM bounded it: its handler reaches `EI`
  1,819 T after INT rises, and its field counter counted once a field with
  INT held 1,664 T and twice at 2,500.
- **Take the snapshot where the next active line begins, not where the flag
  changes.** Games erase and redraw during the blanking interval after the
  flag falls. The Atom's first snapshot, at the flag's rise with a guessed
  6 % low time, caught a game's redraw half done every other field, and its
  objects flickered out. Use the video chip's datasheet line counts (for an
  MC6847, 262 lines: 192 active, 32 with FS low, 38 blank).

**Take line numbers from the circuit, not from a reference emulator.** The
Ace was first built with MAME's numbering, under which INT fell 120 lines
before the display. The schematic put it 64 lines before: MAME's numbers say
where it places the picture in its bitmap. A test redraw sized to the wrong
window passes on the emulator and would tear on the machine.

Test the rules with a guest loop on the host: it must observe the flag low
and escape once per field, and a redraw started after the flag falls (or
the interrupt is taken) must be finished in every snapshot. Run the old,
wrong field shape as a **control that must fail**, so the test is known to
tell the two apart.

### 5.7 Golden images

Render fixed VRAM in every mode and colour set to PPM files and commit them.
**A golden image proves nothing until someone has looked at it**: it is
generated by the code it checks. View every changed image before committing;
on a mismatch have the test write `<name>.actual.ppm` for inspection, and
have a one-pixel change fail the test at that pixel.

### 5.8 A font for the emulator's own pages

Menus and the About page need a font that does not depend on what a program
has done to guest memory. **Use the guest's own character set, taken from
the ROM.** The Ace's menu first used a public-domain 8×8 font kept in the
tree; on the panel it was hard to read, and the owner asked for the Ace's
own. It is now expanded at start-up from the ROM's tables, as the ROM's
power-on code expands them (block graphics computed from their codes,
printable glyphs read from a packed table), not read from character RAM, so
a program that redefines a glyph cannot corrupt the menu. A host test
requires the expansion to equal what the ROM writes into character RAM, in
every machine size. The menu is then in the guest's own case and style.

A font kept only for a test is dead weight: the Ace's 8×8 font went from
the tree before release.

---

## 6. Audio

### 6.1 A 1-bit speaker: integrate, do not sample

Point-sampling a speaker bit aliases audibly. Treat each output sample as the
**time average of the speaker level over the guest cycles it covers**, a box
filter at exactly the sample period:

```
on every write that changes the speaker bit:
    acc += level * (now - last_change); level = new; last_change = now
at each sample boundary:
    acc += level * (boundary - last_change)
    sample = acc / cycles_per_sample; acc = 0; last_change = boundary
```

It costs an add and a multiply per toggle, not per sample, and is exact for
square waves of any frequency.

- **Keep the sample period as a reduced rational** of guest cycles (2048/75 at
  a 1 MHz guest and a 150 MHz host; 6656/75 for a 3.25 MHz Z80) and advance
  the boundary by quotient and remainder. No division, no accumulated
  rounding: after any run the sample count is exactly `⌊cycles × den / num⌋`.
  The host's sample rate is itself a rational,
  `clk_sys / (divider × (TOP+1) × oversample)`; never feed the truncated
  integer back into timing (HW §5.2).
- **Take a wrapping 32-bit cycle counter by difference**, and test the sample
  count across the wrap.
- **Stamp an edge with the cycle count at the start of the writing
  instruction.** The offset to the actual store is the same for every edge a
  loop makes, so pitch is exact and only phase moves. Where the CPU adds an
  instruction's cycles after its bus accesses, as the Ace's Z80 does, the
  counter at the access already is the instruction's start. A stamp at the
  instruction's end failed the Ace's 1 LSB check by 2,031.
- **Follow it with a one-pole DC blocker** (pole ~0.995), so a speaker left
  high and one left low both settle at 0. Then 0 is the silence value, which
  is what an underrun and a parked guest emit.
- **Fixed point throughout.** No `double` literals near it (HW §2.2).

Test it against an **independent model** built from the edges the guest
actually made, every sample to 1 LSB, and measure the pitch of a ROM routine
(the bell) against the cycle count of its loop. The Atom's read 387.64 Hz
measured against 387.60 Hz computed. On the Ace, every half period of the
ROM's `BEEP` equalled the hand count of its loop (13m + 2 T) at three
pitches, and the pitch off the output matched to 1 part in 10⁴.

**Find out on the host whether the ROM clicks.** Where every port access
moves the speaker (§4.2), a keyboard scan may make a tone. The Ace's prompt
is silent: its scan is all `IN`s, so after the first the level stays put.

A guest with a sound chip is a synthesiser instead; HW §5.5 has what a
software PSG costs.

### 6.2 Plumbing

Follow HW §5.3–5.4 exactly: two chained DMA channels, a power-of-two aligned
ring with the hardware read wrap, both read address and count reset on
re-arm, `DMA_IRQ_0` at priority `0x40`, refill path in SRAM. At oversample 2,
each frame occupies two ring slots; size the ring for that.

Between the producer and the IRQ put an **SPSC queue of finished PWM compare
words** (convert at push time so the IRQ only copies), ~28 ms deep, starting
playback at ~21 ms. Producer and IRQ are both on core 0, so it needs no lock.

**Two counters, not one** (HW §5.8): PCM underrun samples (producer starved)
and late DMA refills (IRQ starved). A refill is late when its channel is
already running again on entry; leave it alone and take nothing from the
queue, or the samples will be overwritten before they play. **When muted,
keep producing and consuming**, so mute does not change timing.

### 6.3 Pace the guest on the audio queue

The emulator runs faster than real time, so something must hold it back.
**Block core 0 until the PCM queue has room for another field's samples**
(wait with `__wfi()`; the DMA IRQ on the same core wakes it). The PWM wrap is
a hardware clock derived from `clk_sys`, the most stable timebase on the
board, and audio and CPU then share one clock by construction and cannot
drift.

Pacing does not make underrun impossible: a guest that falls below real time
starves the queue by construction, and a flash erase stops everything (HW
§7.2). Keep the underrun counter live, emit silence, and resync without
replaying. For a build with audio disabled, pace on `time_us_64()` against an
**absolute** field deadline, so a late field does not accumulate. Bring the
machine up on that timer pacing first, and move to audio pacing when audio
arrives; keep the timer build as the control for audio's own cost.

The control quantity for every measurement on the machine is **samples
consumed per second against `time_us_64()`**: it should read the nominal rate
(36,620–36,621 Hz here) whatever the guest does. While turbo tops the queue
up (§9.3) it wobbles by a few hundredths of a percent per window, and comes
back when turbo stops.

**Audio's cost may be in the cache, not in its cycles.** With the Ace's
interpreter in flash, audio cost 0.4–2.0 points of core 0, most of it as
extra host cycles per guest instruction rather than in the beeper's few
calls a field; with the interpreter in SRAM it fell to 0.4–0.8.

### 6.4 Make the failure paths fire

Every counter must be able to fire (§12), and the recovery behind a counter
that has never fired is unproven. Force the rare paths with a scratch build.
The Ace's late-refill path never ran in a soak, so a build that masked core
0's interrupts for 9 ms every 250 fields forced it: each stall counted 3
late refills and lost three halves' worth of samples, with no IRQ storm and
no underrun, and playback carried on. That is evidence the recovery works,
which a counter stuck at zero is not.

---

## 7. Keyboard

### 7.1 The impedance mismatch

A vintage guest scans a key matrix at its own speed and reads modifier lines
separately. The PicoCalc delivers **translated ASCII events over a 10 kHz
I²C bus**, with Shift already resolved by its keyboard MCU and no raw-key
mode (HW §6). So the mapping runs backwards:

```
[state, code] → normalise → held-key set → code → (row, col, modifiers) → guest matrix
```

The rules, every one from measured MCU behaviour (HW §6.1–6.3):

- **Build held-key state from press/release events.** Never consume a
  character stream. Auto-repeat arrives as extra *press* events.
- **Canonicalise** letters and shifted punctuation for held-state identity:
  releasing Shift first gives press `A` / release `a`.
- **Fix the binding at press time and keep it with the held key**, so a
  key's release undoes exactly what its press did even if the layout changed
  in between.
- **Drain the whole FIFO each poll**, from core 1 at ~30 Hz, in thread
  context; the poll also feeds the MCU's 2.5 s bus watchdog.
- **Replay key events into the matrix at the guest's pace.** A ROM keyboard
  routine needs several fields to see a key (the Atom's, ~8 fields per key,
  with no type-ahead). Hold each key down a minimum number of fields and
  leave a gap before the next.

**Settle the hold and the gap by executing the ROM, with controls.** The
Ace's scan takes a key on the third consecutive field that sees it, needs
one field with every key up before the next, and repeats a key held 33
fields. Its replay holds each key 4 fields and leaves 2 up, one more of each
than the ROM needs, for a scan the guest delays: about 8 keys a second. A
test typing at 2 held and 0 up, which loses keys, is the control.
**Modifier releases wait the same minimum**, so a Shift tapped within one
poll still reaches a game that reads SHIFT alone.

### 7.2 The mapping table

Make the mapping **data**, one row per PicoCalc code: target row, column and
flags (assert the guest's SHIFT, assert CTRL, Alt layer, a standalone line,
reset, open the menu). Then a host test can check it: every code maps to
exactly one cell, and nothing binds a chord the MCU cannot deliver.

**Settle the guest's matrix by executing its ROM**, not from a secondary
document. Press each of the matrix's cells at the prompt, with and without
SHIFT, read what the ROM writes to VRAM, and you have the whole map,
including unused cells and keys whose meaning SHIFT reverses. Keep that sweep
as a regression test that types every table entry through the real ROM. The
Ace's sweep, with each of its two shift keys and both together, corrected
two beliefs written from secondary sources: up and down were swapped, and a
key the documents gave as TRUE VIDEO typed a digit.

Guest keys the PicoCalc lacks go on an **Alt layer**, chosen because the
guest probably has no Alt key, so nothing is stolen from it. A key pressed
with Alt down comes from the Alt layer only, so no game layout can take away
the menu or reset.

**A guest with a second shift key** (the Ace's SYMBOL SHIFT): map the host's
Ctrl to it, since Ctrl chords reach the host unchanged (HW §6.3) and so give
raw access to every second-shifted cell. Punctuation the PicoCalc sends as a
Shift chord but the guest types with its second shift (`:` `"` `!` on the
Ace) needs a flag that takes the guest's SHIFT out while that key is down,
so a program reading the matrix sees what a guest typist would leave there.
**Drive the guest's own cursor keys** under the guest's SHIFT, asserted by
the emulator: the host's swallowed Shift+arrow chords then cost nothing.

Constraints from the MCU that bind the table:

- **Shift+Left, Shift+Right, Shift+Space and Shift+Backspace send nothing at
  all**: no press, no repeat, **no release** (HW §6.3). A direction key let go
  while Shift is down stays held in your set. Never make Shift part of a game
  binding that must chord with the arrows. A guest BREAK that is SHIFT+SPACE
  needs a plain host key (Esc, and Break, which is Shift+Esc).
- **Alt+`,` `.` Space `B` are consumed by the MCU** (backlights, battery).
- Several keys exist only as shifted alternates (Home, End, PgUp/PgDn, Break,
  Insert). Insert is both Shift+Enter and Alt+I, so decide which physical
  key it is when its event arrives, by whether Alt is down (HW §6.2).
- `F1`–`F5` arrive as `0x81`–`0x85` and `F10` as `0x90` (the MCU's
  Shift+`F5`). A guest without function keys leaves them free for the
  emulator's own pages.
- A guest modifier on its own is a key games read. The Atom's SHIFT line was
  read alone as a jump button, so the host's Shift must assert it whatever
  else is held. A guest key that acts as a held modifier (the Atom's REPT)
  cannot be an Alt chord: give it a plain key.
- Characters the MCU sends shifted (`|` `{` `}` `` ` `` `~`) need entries of
  their own, mapped to the guest's shifted cells by what they are. Missing
  `|` meant an Atom BASIC operator could not be typed. Where the guest's
  character set differs from ASCII, map by position or by meaning and say
  which (the Ace has `£` at ASCII's `` ` ``, so `` ` `` types `£`).

Latency is one poll interval plus one field plus a 4–5 ms transaction,
~55 ms at 30 Hz. Fine for BASIC and Forth, and for games written against a
ROM that debounced across fields.

### 7.3 Game layouts

Games scan the matrix directly, often with keys laid out for the original
keyboard. Provide **layouts as overlays** on the standard map: a few PicoCalc
keys rebound to guest cells (asserted without SHIFT) or standalone lines,
everything else unchanged so the game's own prompts still type.

- Built-in layouts are **generic and name no game**: named by shape, such as
  "the arrows as the guest's own cursor keys" or "the arrows as Q A O P".
  Per-game layouts are text files on the card, one binding per line, and may
  name the media they go with; loading such a tape or snapshot selects the
  layout, and the menu says so. Loading media no layout names leaves the
  choice alone (a loader may fetch its next part under another name).
- **Name files, not the names inside them.** An Ace snapshot holds no program
  name at all, so its layout files list tape and snapshot file names without
  their extensions.
- Reject a malformed layout file whole and name the line; a partly applied
  layout is a different layout.
- A layout is host state, not guest state: keep it out of snapshots.
- Put fire on a key whose chord with the arrows the MCU delivers. The Atom's
  first cut put fire under the same thumb as the directions; on the device a
  key at the far side of the keyboard (`]`) played better, and the Ace kept
  it.

---

## 8. Media: ROMs, tape, disc, snapshots, settings

### 8.1 ROMs

- **Ship no ROM images unless a permission covers them.** Without one, the
  user puts them on the SD card; the README says which files, where to get
  them and their SHA-1s. The Atom works this way.
- **Where a permission exists, embed the ROM** in the firmware, as the Ace
  does under a 1998 statement from the company that bought Jupiter
  Cantab's remaining assets. It is informal and does not show that the
  copyright passed, but Ace emulators have relied on it for over 25 years. Keep the ROM, its permission and an
  attribution entry together; say it is not under your project's licence;
  write down the fallback (the card, by SHA-1) in case a rights holder
  objects. **Fail the build if its SHA-1 differs**, so a damaged or
  substituted file cannot boot and then misbehave. Embedding deletes a class
  of problem: no missing-ROM page, no near-miss dump, and the host tests that
  run the real ROM never skip, in CI too. A ROM no permission covers (the
  Ace's disc ROM, needed only by a reference emulator) stays out of the
  repository, ignored by git.
- **Identify ROMs by SHA-1**, and know the common alternate packagings (two
  4 KiB images that one collection ships as a single 8 KiB file; the Ace's
  ROM is the two 4 KiB halves MAME lists, concatenated). A near-miss dump
  boots and then misbehaves, the most expensive class of bug.
- **A missing ROM shows a page naming the missing files**, never a blank
  screen. On this hardware a blank screen is the single most expensive
  failure to debug.
- Show every slot's file, status and hash on an About page, so a bug report
  can be copied off the panel.

### 8.2 Tape, phase 1: trap the OS routines

Fast and simple: when the CPU reaches the ROM's load or save routine, serve
the request from a file and return as the routine would have.

- **Trap the handler, not the public entry point.** If the entry point jumps
  through a RAM vector, trapping the handler means anything that repoints the
  vector (a disc OS, a utility ROM, a game's own loader) is never trapped.
  On the Ace, trap the routines that read and write one block (a header or
  the data), and leave the words around them (name search, messages, length
  checks) to the ROM, which calls again for the next block as it would with
  a recorder.
- **Check the handler's first bytes** against the stock ROM's and stand aside
  for any other ROM; what you reproduce is that ROM's contract.
- **Stall the CPU, do not return early.** At the trapped boundary the CPU
  behaves as if RDY (or a Z80's WAIT) were held low: guest time passes,
  devices tick, no instruction runs, and the run loop keeps its contract
  while the port serves the request at the next field boundary (§2.5). A
  request no file answers is **declined** and the ROM routine runs as if
  there were no trap, so the user sees the ordinary "PLAY TAPE" prompt.
- **Leave what the ROM leaves, byte for byte**: parameter block pointers,
  the last block header, checksums, mode bits, registers, flags, I/O latches.
  Code after a load (`*RUN`, the BASIC re-entry, a game loader) may look at
  any of it. Test this by running the ROM's own routine, with only its
  byte-level cassette routines hooked, and requiring the trapped call to leave
  the same machine in every byte below ROM.
- **Better, let the ROM finish its own routine.** The Ace's trap does not
  return at all: it puts the machine where the block routine is as it reads
  the last byte (or sends the checksum), with the stack that routine leaves,
  and the ROM runs its own epilogue: the checksum test, the flag check, the
  BREAK test, `EI`, `RET`. What the routine leaves is then the ROM's own work.
  Name the deliberate differences (`R`, which counts instructions the trap
  did not run; a block shorter than the ROM asked for).
- **A planted bug that passes marks dead code.** Of three planted in the
  Ace's trap, two changed state the ROM's epilogue overwrites anyway.
- **Find the file the user meant.** Archive files are seldom named after the
  program inside (the Ace's `tut-tut.tap` holds `TUTTUT`): with no file of the
  name asked for, play the first tape whose first header carries it. A load
  that reaches the end of a tape rewinds once, as a user would, and is
  declined the second time. Skip the `._` files macOS writes beside every
  file it copies to the card (HW §7.1).

### 8.3 Tape, phase 2: the signal

Decode a tape image (UEF, CSW, the Ace's `.tap`) into **half-cycles clocked
in guest cycles** and present them on the input bit the ROM reads, brought up
to date only when that port is read (§4.3). It works with any loader,
protected or headerless, and turbo is free.

- **Read the ROM's tape routines before writing the decoder**: how it times a
  bit, what it counts, what leader it waits for. The Atom's timed writes
  against a hardware 2.4 kHz reference input that had not been modelled; a
  save that the phase-1 trap declined would have hung.
- Carry remainders when converting tape units to guest cycles so a half-cycle
  is 208 or 209 cycles and never drifts.
- Decompress gzip straight into the flat buffer, one bit at a time as `puff`
  does; back-references read from the output itself, so no separate window.
- **Follow the ROM's own cues for the motor.** Stop at the "PLAY TAPE" prompt,
  start on the key that answers it, stop when the load returns (except for a
  load that runs code which may read on). Find those PCs with one table
  lookup on the low byte in the run loop. On the Ace, the load routine's
  entry starts the deck, the save routine's starts the recorder, and the exit
  both share, which every way out passes (BREAK included), stops both. A
  recorder that follows the save cue needs no Record control.
- **Read semantics off the ROM, not off folklore.** An empty file name was
  "the Atom ROM's nameless format", not "the next file". Find a cue's exit by
  reading the code: the switch-off we first chose ran after every block, not
  once per file.
- **Recording** is the inverse: decode the output bit into standard chunks
  appended to the tape, a byte at a time, keeping the image valid after every
  byte. Leave out the writer's pause between bytes (a 48-byte Atom save went
  from a 748-byte image to 104). Drop and count bytes that do not frame. A
  gzipped or read-only image is write-protected. Where the format is whole
  blocks, **keep only whole blocks**: one cut short by BREAK, a reset or a
  full buffer comes back out, counted. A reviewer found the Ace's first
  recorder keeping them.
- **When the ROM's writer times bits with its own loops, play its counts.**
  Count the T-states of each half-cycle off the writer's instructions,
  every branch that changes one (the first bit of a byte, the checksum,
  the end of the leader), and have the player give exactly those. A test
  can then require the player's edges to equal a recording of the ROM's
  own save, edge for edge, which catches a count one T out in any branch;
  a tolerance-based check would not. The Ace's held 9,924 edges and the
  gap between header and data.
- **Settle which bit carries the tape, and its polarity, by execution.** The
  Ace's speaker follows the port access while its tape output is D3 of each
  `OUT`; recording the ROM's save, the access line decoded to nothing and D3
  to the file the trap writes. Play the input at the polarity that leaves the
  line at the input's idle level between blocks.
- **One trap, two speeds.** With fast loading off, the Ace's trap still
  stalls at each block routine as the port's cue and finds the file exactly
  as it would, but then loads it whole into the signal buffer and declines,
  so the ROM reads the signal. A block the buffer cannot hold is saved by
  the trap instead, and a recording waiting for a missing card is kept until
  the card changes.
- **Settle clock questions by execution.** The Atom's ROM writes a tape
  correctly at 2 MHz (it times bits against a reference that keeps wall time)
  but cannot read one (it times input with its own loops). The deck therefore
  plays only at the stock clock, and says so.

Turbo (§9.3) loaded an Ace tape off the signal at 3.18–3.19× real time.
Core 1 does not present every field while it runs, and its dropped
snapshots are expected there.

### 8.4 Disc controller

- **The model knows the drive geometry and asks the host only for bytes**, a
  track at a time: a read posts a request and the chip stays busy while the
  CPU runs on; a write collects sectors and then posts. The host serves it
  with the guest parked (§2.5), so the card's 17–27 ms per track is invisible
  to the guest.
- **READY is how a DOS notices a disc change.** The Atom's DOS re-read the
  catalogue only when the drive was not ready, which on real hardware
  followed the head unloading after an idle count. Model the head load and
  unload, and run a pending unload when the user swaps discs with the guest
  paused, or the old catalogue persists.
- Read the register addresses off the DOS ROM, not secondary documents; the
  Atom's were not where the old document put them, and the interrupt was NMI.
- Short images (truncated after the last used sector) read zeros past their
  end and grow when written. A read-only file is a write-protected disc.
- Refuse a snapshot while a command is in progress.

### 8.5 Snapshots

- **Write fields explicitly, little-endian**, never a struct dump: the
  machine struct holds pointers and padding, and a snapshot must outlive the
  build that wrote it.
- **Header with magic, version, lengths and a CRC.** **Encode every field so
  zero is its reset state** and reserve bytes; then a later version can fill
  reserved bytes and an older file still loads as an idle device.
- **Store ROM hashes, not ROM bytes**, and refuse a machine whose ROMs or
  memory map differ. Record the guest clock and refuse the other one.
- **Record every timing-model choice** that changes how the machine runs (the
  field's shape, wait states on or off) and refuse a state made under
  another. When the Ace's field moved to the circuit's numbering (§5.6),
  every earlier state became one that is refused, not misloaded.
- **Load in two passes.** The first reads and checks everything without
  touching the machine; only then does the second change anything. A torn,
  foreign or newer file leaves the running machine exactly as it was.
- After a load, release all keys and restart audio from the restored clock.
  The beeper's sample grid is not state: the speaker's edges come out the
  same, and the samples within one of them.
- Take states between fields, where the guest is parked; the field resumes
  from its first active line and the debt carries the overshoot.
- Test by execution: save mid-program, restore into a machine that has been
  doing something else, run 150 fields, and require identical state (RAM,
  CPU, cycle counter, speaker, debt). A debt one cycle out must not match.
- **Log destructive actions.** An Ace run over the UART, with no files on the
  card, stopped on Delete and removed a save slot without a word.

### 8.6 Writing files to the card safely

Every write goes to `name.new`, which is closed, then the old file is unlinked
and the new one renamed into place. Rename is not proof of power-loss
atomicity (HW §7.1), so put the **recovery on the load side**: a file that is
missing or fails its check gives way to a whole `.new`. Do all card work at a
defined boundary with the guest parked (HW §7.1).

### 8.7 Settings: a text file on the card, not flash

Keep the user's power-up configuration in a `key = value` text file on the
card, and **write no internal flash at all**:

- One source of truth that the user can read and edit on any computer, and
  that survives reflashing.
- A flash erase takes tens of milliseconds with XIP offline, longer than the
  audio deadline (HW §5.4, §7.2). A card write has no such cost.

Rules that worked:

- **Every default in one function.** The file names only what it changes.
- A wrong line changes nothing and the lines after it still apply; the first
  problem goes to the menu's status row. A **duplicate key is an error**, so
  nothing depends on line order.
- **Saving is a deliberate menu action, never automatic.** Trying a setting
  costs nothing until saved.
- **Edit the user's text, do not regenerate it.** A key's line keeps its
  place, indentation and trailing comment; only the value changes. Keep
  comments, blank lines, unparsed lines and the file's line ending. Append a
  missing key only if its value differs from the default. A value that
  already says the same keeps its spelling, so a second save changes nothing.
  A line whose value the parser refused is that key's line, unless another
  line gives the key a good value: the save writes the value in force over
  it, and if another line does, makes it a comment, keeping its text. So
  saving clears the problem the status row names rather than keeping it
  for ever. (The Ace's owner found the old rule on the device: a refused
  line survived every save.)
  Keep comment columns aligned. Refuse a save when the file has a duplicate
  key.
- **Parse your own output before writing it**, and refuse if it does not give
  the settings back. A rewrite that disagrees with its own reader is a bug,
  caught on the board as well as in tests.
- Read the file before anything that depends on it: the host clock (§9.4)
  is read on core 0 before stdio or any peripheral is up, and a RAM size
  before core 0 powers the machine on.
- A value the user may set outside the emulator (the backlight, which the
  MCU's own chords change) can be left unset, meaning "leave it", and a save
  keeps what the file has.
- Build-time overrides (`-DBOOT_TAPE=...`, `-DBOOT_CLOCK=...`) win over the
  file, so a run driven over the UART knows what it booted with (§13.2).

### 8.8 Importing a community snapshot format

An archive's snapshots are often in a format some old emulator invented,
documented after the fact. The Ace's `.ace` is one. Settle it from three
sources together: **the community's description, a reference loader's
source, and a corpus of real files** (199 for the Ace: one archive's and a
TOSEC set). Then write the format down, with what the corpus showed:

- **Real files carry noise.** The Ace's registers are stored as 32-bit words
  of which only the low 16 or 8 bits mean anything; the rest is whatever the
  writer's memory held.
- **Files may not hold the whole machine.** No `.ace` dumps past `$7FFF`, so
  a file saved on a larger machine lacks its top of RAM, which holds the
  Z80's stack.
- **Reconstruct only what the corpus proves.** Of the 135 Ace files saved in
  the ROM's key wait whose stack top is in the file, every one has the same
  single word there, which is also what the host ROM leaves at its prompt.
  So a file saved in the key wait without its stack top gets that word
  written back; a file saved running without it is refused (one, of 199).
- **Archive tags may not mean what they seem.** TOSEC's `[3K]` is the RAM a
  program needs, not the machine it was saved on: 76 of those were saved
  from 19K machines. Read the machine off the file's own state.
- **A file for a machine you do not offer** loads into the next one up if
  its bytes need no change (35K files into the 51K machine), and any other
  mismatch is refused, naming the machine needed.
- **Check against a reference emulator, run headless, over the whole
  corpus** (§11.4), leaving out what depends on where in a field each loaded:
  the PC within a wait loop, per-field counters, the bytes below SP where
  interrupts push. The Ace's 102 comparable files agreed in everything else.

Treat export as a separate decision. Import covers the archive, and your own
format (§8.5) covers the user's saves.

---

## 9. Timing and clocks

### 9.1 The guest's field

Pin the field rate and line counts from primary sources and make them
constants once settled (§14.2). The Atom's field rate was configurable while
unverified; it turned out to be 60 Hz on every unit sold, including those in
50 Hz countries, and software timed itself on it. The Ace's field (312 lines
of 208 T), first active line, INT line and INT length were bounded by
executing the ROM and then settled in one reading of the schematic, and
became constants then.

### 9.2 Power-on state

**Zero-fill RAM** at power-on rather than modelling a chip-dependent
checkerboard no software can rely on, with one exception to look for: **state
the software uses as a seed.** The Atom's BASIC random generator was a shift
register that never leaves zero; from zero-filled RAM `RND` returned 0 for
ever and a game drew all its asteroids at one point. Seed such state from the
board's hardware RNG in firmware, and from a constant in host tests so runs
repeat. Look, and record what you find either way: the Ace's ROM has no
random-number word at all, and the manual's `RND` is user code seeded from a
field counter, so zeroed RAM leaves nothing stuck.

### 9.3 Turbo and faster guest clocks

**Turbo** (run unpaced) while a tape plays: drop the guest's samples and top
the audio queue up with silence to its start depth without blocking. Because
the tape is clocked in guest cycles the guest cannot tell; a 300-baud Atom
load finished 2.7–2.8× faster, and an Ace load off the signal 3.2×. Keep a
paced build as the control.

**A faster guest clock** (an owner's modification on the original) needs a
rule for every timed quantity:

| Follows the CPU clock (same cycles, faster in wall time) | Fixed in wall time (more cycles at a higher clock) |
|---|---|
| timers clocked by the CPU's Φ2 | the video field and its line split |
| software delay loops (the bell goes up an octave) | the audio sample period |
| | references from their own crystal, tape half-cycles |
| | disc controller byte, sector, step times |

Compute the second column from the clock once at init and keep each value
where it is used. Change the clock only through a power-on restart, never
under a running program; converting every in-flight count is complexity for
nothing a user notices. Run every clocked host test at each guest clock
(§11.2). Where no common modification exists (the Ace), drop the option and
the table goes with it.

### 9.4 The host clock

Changing `clk_sys` moves five other things (HW §3). For an emulator the safe
shape is:

- `main()` starts at 150 MHz, reads the settings file on core 0 with nothing
  else up, and **only then** moves to 300 MHz if asked: rail to 1.20 V first,
  the flash's QMI divider and RX delay doubled from SRAM so flash stays at the
  bootrom's 50 MHz, then the PLL, then `clk_peri` onto `clk_sys`. Only then
  start stdio, I²C, the LCD and audio, so every driver derives its rate from
  the clock it finds.
- Derive the audio PWM divider as `clk_sys / 150 MHz` so the sample rate is
  identical at both clocks.
- **Set the rail back down at 150 MHz explicitly**, even in a firmware that
  never raises it. The regulator survives a reset (HW §3), so a board last
  run by another firmware at 300 MHz would otherwise keep 1.20 V.
- **Change the host clock by writing the setting and restarting with the
  watchdog**, not by retuning running peripherals.
- At 300 MHz, host cycles per guest instruction were unchanged: CPU-bound code
  is clock-bound even with flash at 50 MHz, and a 4 MHz guest at 300 MHz left
  core 0 the same margin as 2 MHz at 150. Soak it on battery before calling it
  stable, and put the die temperature on the heartbeat.

If the CPU-alone gate (§14.3) passes at 150 MHz with a wide margin, as the
Ace's did at three times, defer 300 MHz entirely and build for one clock.

---

## 10. The user interface

- **Boot straight into the guest.** The emulator is invisible until asked.
  Know what the guest's own power-on looks like: the Ace shows a blank
  screen and a cursor, and prints `OK` only after a line runs.
- One menu key (an Alt chord) opens a menu that **pauses the guest** (§2.5).
  Function keys open its pages directly from the running guest, and closing a
  page so opened returns to the guest. Inside the menu the function keys do
  nothing.
- **Draw the menu as a guest text page through the guest's own renderer.**
  It costs almost no code. Use the guest's own font (§5.8). Closing it
  **does not restore pixels** (the shadow holds VRAM bytes, not a panel
  image); invalidate and redraw the next snapshot whole, one full present.
- **A family of emulators shares one menu.** The Ace's owner decided its
  menu is the Atom's, item for item and key for key, less what the Ace
  lacks: F2, the Atom's Discs page, does nothing on the Ace rather than
  being given another meaning. A user moving between the two finds
  everything where it was.
- A **machine page stages** changes (RAM, clock, ROMs, host clock), marks
  staged rows, and applies them only by an explicit **restart that behaves as
  a power-on**. Check the card's ROMs in a first pass before touching the
  machine, and refuse rather than leave a broken one. Warn that the program in
  memory is lost. Refuse while an unsaved recording exists.
- **Pause** on a key: park the guest, dim the backlight (read the current
  level first, since it may be the MCU's own, and restore it), show `PAUSED`.
  Any key resumes and is not passed to the guest; a modifier alone does not
  resume, and neither does the pause chord's own auto-repeat.
- A **New tape** item that makes an empty, numbered file and puts it in the
  deck gives the user somewhere to save without a computer.
- An **About page** with the firmware version, physical board, chip revision,
  clocks, southbridge version, die temperature, every ROM's hash and the
  settings file's state. Generate the version from `git describe --always
  --dirty` **at build time**, rewriting the header only when it changes; a
  configure-time value goes stale on the next commit.
- A **status row** in the menu names the first problem: a bad settings line, a
  missing ROM, a refused save, a protected tape, which file chose the layout.
- **Firmware names no specific titles.** Built-in behaviour is generic; per-
  title configuration lives in files on the card that the user writes.
- Keep the user's port (GPIO) features honest about what they cost: on the
  development build the UART owns GP4/GP5, and the menu says which of the log
  or typed keys is lost if the user takes them.

---

## 11. Testing

### 11.1 Shape

- **No framework.** A header with `CHECK` and `TEST_DONE`; one binary per
  area under CTest; exit code 77 is a skip.
- **Prefer asserting behaviour by executing it** over comparing two tables in
  the same repository.
- **Give a test a control that must fail**: run the old, wrong behaviour
  alongside and require the test to tell them apart.
- **Plant bugs to prove each test bites**, one at a time in a scratch copy,
  and record which failed. A plant that passes is either a gap in the test
  or a change nothing can see (§8.2).
- Keep hardware-independent logic (dirty tracking, the snapshot pool's state
  machine, status-line formatting, settings rewriting, parsers) in the core,
  where the host tests reach it.

### 11.2 Run the real software on the host

A small harness builds the real machine on the workstation: it finds the
ROMs by SHA-1 (in a gitignored staging directory, or in the repository when
they ship), types keys through the same matrix path the firmware uses, and
reads results out of VRAM. Tests that need absent ROMs skip; tests of an
embedded ROM never do, and CI runs them on every push. With it, the
regression suite runs the actual OS, BASIC or Forth: boot, type a sum, ring
the bell and measure it, save and load through the ROM's own routines, run
the DOS against an in-memory disc, restore a snapshot mid-session, load
every snapshot in an archive.

Tests whose subject has a clock are registered once per guest clock with an
environment variable the harness reads.

**Read results as text.** A dump of the guest's screen RAM, decoded through
its character set, is how a host test, a scripted device run (§13.1) and a
person over a serial line all read what the ROM printed.

### 11.3 Media round trips by the ROM itself

For tape: record what the ROM's own `SAVE` drives onto the output bit, decode
it with an independent decoder in the test, write it as an image, and load it
back through the ROM's own `LOAD`. Keep the test's decoder as the independent
model the core's recorder is compared against. Then load a recording in a
reference emulator (§11.4): an Ace recording made on the device loaded and
ran in xAce.

### 11.4 Trace-diff against a reference emulator

Run the same ROMs and key script under your core and under an established
emulator, print the registers and cycle count plus the opcode bytes before
each instruction (`PC A X Y S P cycles` for a 6502; `PC AF BC DE HL IX IY SP
T` for a Z80), and diff. The first divergence is almost always the bug, and
it finds problems no unit test is shaped to catch.

- **Get the reference building early**, in the milestone that first boots
  the machine on the host, and keep it in use. The Atom left "a recording
  loads in another emulator" to its last milestone, and that check is still
  outstanding because the other emulator could not be got working. The Ace
  built xAce in its third milestone, and every later check found it ready.
- Build the reference from its own checkout, at a pinned commit, by a script;
  copy none of it into your tree. Patch it as little as possible: the Ace's
  script adds one line by `sed` (a trace hook at the top of xAce's CPU loop,
  where its registers are locals) and replaces its X11 front end with a
  `main` of its own, so no GUI headers are needed. Keep the harness a tool,
  not a CTest, since it needs that checkout, but CI can clone and run it on
  every push.
- Two emulators keep different time (field lengths differ), so polling loops
  run different counts. Either **resync** at the next point where registers
  and a shadow return stack agree, classifying what lay between (a
  divergence right after a read of a timed input is expected; different
  values, a different path, a lost trace or an unexplained cycle difference
  fails), or, where the reference's timing is broken anyway, **make the two
  keep the same time**. xAce's interrupt is a wall-clock `SIGALRM`, so the
  Ace's driver raises INT over the same window of each field in both, adds
  the acknowledge's 13 T that xAce does not count, and corrects its timing
  errata. The traces then agree line for line, and every difference left is
  in a named class.
- **The reference has bugs.** The Atom's found six cycle-count errors in its
  reference and none in our core; the Ace's found xAce timing `LD r,n` at 4 T
  instead of 7 and `RES`/`SET b,(HL)` at 12 instead of 15, setting N in
  `ADC HL,ss`, never setting S in `BIT`, and not modelling flag bits 3 and 5.
  When they disagree on cycles, check the manufacturer's table before
  believing either. Carry the reference's known errata in the tool, by name.
- Know where the comparison must stop. xAce's `HALT` is a NOP, so the Ace's
  diff ends at the first `HALT`; turn off in your core what the reference
  lacks (wait states) for the diff.
- **Use a second reference for what the first cannot do.** xAce has no
  snapshot loader, so the Ace checks its snapshot import against MAME, run
  headless with a Lua script that dumps registers, screen and RAM after a
  fixed number of fields (§8.8). MAME's driver wanted ROMs that were not to
  hand (a speech chip's); a zero-filled stand-in let it start.
- Plant a bug (a wrong bit in a port's read-back; a taken `JR` at 13 T) once
  to prove the harness catches it. The Ace's was caught at the fourth
  instruction.
- Keep typed lines under the guest's line length, and wait after RETURN; the
  guest may have no type-ahead.

### 11.5 The soak

A 30-minute run **on battery** with a guest program that exercises display,
sound and keyboard together, with the log captured throughout. A script then
checks the log: one boot, heartbeats covering the run, real-time ratio never
below 0.995, and every failure counter zero on every heartbeat (I²C errors,
keys lost, underruns, late refills, undocumented opcodes or `ED` holes,
dropped snapshots, log lines dropped), while presents, key events and
speaker edges grow.

- The program must not stall on a missed key: have it read the matrix itself.
- Type keys at it for the whole run, and choose them carefully: the Atom's
  first soak stopped because a typed `R` was read as Escape by BASIC's
  escape test in the matrix column the program had left selected.
- **The check must show the workload kept running.** Counters at zero prove
  nothing if the program stopped. The Ace's check requires the program's
  count to rise from each screen dump to the next, the speaker to move in
  every heartbeat, and the dumps to show the program reading each typed key.
  A first Ace soak with every counter zero had dumps showing only one of the
  two keys read; the check was tightened and the soak run again. A later
  release soak failed on the keys alone, and was recorded as such.
- **Keys typed over the UART bypass the southbridge**, so a UART soak does
  not exercise the real keyboard. Soak the release build too, with no UART:
  keys held by hand on the PicoCalc and the counters read over SWD with both
  cores running (HW §2.7). Read the counter block twice and keep a sample
  only when both reads agree: an Ace sample read while core 0 rewrote the
  block showed two windows a tenth fast and slow.
- Record the power source: the southbridge's charging bit proves USB power;
  its absence does not (HW §6). The Ace's release soak read the die at
  20–21 °C throughout, on battery, at about 21 % of core 0.

---

## 12. Measuring

HW §9.1 has the discipline: profile **in the mode you ship**, carry a
**control quantity** that should not change, expect **~2 % run-to-run
spread**, compare **on one board**, and **write numbers to a file**. For an
emulator specifically:

- **Report headroom, not just real-time ratio.** A paced emulator reads 1.000
  whatever the code costs. Measure the share of core 0 spent inside
  `run_field` and guest cycles per microsecond of it, plus host cycles per
  guest instruction.
- **Bench the CPU alone first.** A firmware image with only the CPU on a flat
  bus, running a slice of the instruction suite and a loop shaped like the
  guest's software (for a Forth machine, its inner interpreter), reports host
  cycles per guest instruction. Run the same image's workloads on the host
  and require the board's cycle and instruction counts to equal them. The
  Ace's eight runs per image agreed to 0.01 %.
- **Script a few workloads** (idle at the prompt, a compute loop, a scrolling
  loop, a sound loop, and one for each feature with a cost of its own, such
  as character-set animation or printing through wait states), one boot
  each, typed over the UART a line at a time (the Ace's ROM drops keys that
  arrive while it handles the line before), and reduce the heartbeats to one
  line per workload. On the Atom, idle at the prompt was the heaviest; on
  the Ace, whose prompt spins on a flag, compute was. Find out which.
- **Count `HALT` repeats apart from instructions.** On the Ace, `VLIST` halts
  once a word; counted as instructions, the repeats made the mean 4.26 T per
  instruction and hid where the time went.
- **Measure every feature against a control build** with only that feature
  removed, in the same sitting. That is how a 3.5 % port hook, a 2.5–4 % VIA
  regression and its fix, and a recorder hook costing nothing were told apart
  from layout noise on the Atom, and the tape traps (0.7–0.9 points), wait
  states (≤ 0.5) and audio (0.4–0.8) on the Ace. Code layout alone moved
  results by about 1 % on the 6502. With a Z80 interpreter left in flash,
  four builds differing only outside it read 31.8–41.1 % of core 0 on one
  compute loop, steady within each run: the XIP cache's mapping decides it.
  Measure placement tiers before any other change, and compare others
  only at the tier you ship, where one SRAM image read the same to 0.1
  point in three sittings.
- **Measure the previous release in the same sitting.** The Ace's 23 % run
  loop regression (§3.4) and a 27 % idle figure first blamed on three
  unrelated milestones were both found only that way.
- **Heartbeat contents**: real-time ratio, core 0 share and headroom, host
  cycles per instruction, mean cycles per instruction, `HALT` repeats
  skipped, longest present, presents/full presents/dropped snapshots,
  underrun samples, late refills, queue depth and low water, samples
  consumed per second (the control), I²C errors, key events dropped,
  undocumented opcodes or `ED` holes, log lines dropped, the share of guest
  time held by wait states, battery and charging, die temperature. Every
  counter must be able to fire; one that cannot is dead code (§6.4).
- An **on-panel perf line** is for watching; the heartbeat is for measuring.
  Measure the perf line's own cost with it on and off.
- **A release build with no UART still has figures.** Copy the counters once
  a second into one block at a fixed symbol and read it over SWD without
  halting (HW §2.7).

---

## 13. Working on the hardware

### 13.1 The loop

With a Debug Probe's SWD and UART both connected (HW §2.7):

- **Capture the UART first, then flash**, so the boot banner is in the log.
  Let only one process read the serial port: two readers split the byte
  stream and both logs come out scrambled. Make the capture script refuse a
  port something else holds.
- **Flash with `reset halt` then `resume`**, never `reset run`, or core 1 can
  be lost (HW §2.7).
- **Keep a Mac's display awake** (`caffeinate -d`, not `-s`). A sleeping
  display wedges the probe until it is replugged; it cost the Ace two
  unexplained drop-outs before it was found (HW §2.7).
- **Type at the guest over the UART.** The firmware turns received bytes into
  PicoCalc key events, so a run needs nobody at the keyboard. Pace characters
  to the guest's keyboard routine (0.25 s each for the Atom's). Reserve bytes
  for host actions: play/stop the tape, dump the screen to the log, park the
  guest to test card handling.
- **Reach the menu over the UART too.** The Ace reserves one control byte to
  open the menu and another to pause; while either is up, the UART's bytes
  are its keys, with control codes for the arrows (`^P` `^N` `^B` `^F`). Then
  menu actions (save a snapshot slot, change the machine, make a new tape)
  can be scripted.
- For what the menu should not be needed for, give the build **boot-time
  options** that preload media and override settings (`BOOT_TAPE`,
  `BOOT_DISC`, `BOOT_CLOCK`, `BOOT_RAM`, `BOOT_HOST_MHZ`, `BOOT_NEW_TAPE`),
  each in its own build directory.
- **A build directory keeps its cached options.** When a default changes (the
  Ace's SRAM tier), a directory configured before keeps the old value until
  it is passed once. Say so wherever the default changes.
- UART logs have CR line endings and may contain UTF-8; some `grep`
  replacements print nothing on them. Read them with Python if `grep` comes
  back empty.

### 13.2 Two builds

- **Development**: stdio, the log and typed keys on UART1 (GP4/GP5). Every
  tool above needs it.
- **Release**: no UART at all; logging compiles to nothing, UART keys are
  compiled out, and GP4/GP5 are free for a user port. Check it on the panel,
  and read counters over SWD (HW §2.7). One build switch sets the defaults
  that differ, so the settings defaults and the settings rewriter agree.
  Run it on a second board as well: the Ace's release build was checked on
  a Pico 2 W at most milestones while development ran on a Plus 2 W.

### 13.3 Bring-up

Bring up in HW §10's order, each stage its own smoke test: southbridge,
LCD with a corner-coded test pattern (check orientation, colour order and
all four corners by eye), keyboard, audio, card. Log **physical board
identity separately from the SDK build target**; a `pico2` build running on a
Plus 2 W (RP2350B) picks the wrong ADC input for the temperature sensor unless
it reads the package at run time (HW §8.1). Log every key press and release
with its code during bring-up; the Ace's capture of an Alt-first release
became a test case.

With drivers reused from an earlier emulator (§14.6), bring-up is
re-verifying on the new tree, not re-deriving: the Ace's took a day.

### 13.4 Protect the user's files

Scripted device runs write to the card. Use a card, or a directory and
files, set aside for testing, and have the tools name what they write. On
the Ace a test `SAVE` went onto one of the owner's own tapes by mistake, and
its 40 bytes had to be cut off by hand.

---

## 14. Process and documentation

### 14.1 Documents

| Document | Authority on |
|---|---|
| hardware notes | the host: wiring, protocols, timing, measured costs, quirks |
| lessons (this guide) | what earlier emulators taught: architecture, accuracy, testing, process |
| design | the guest and the shape of the code: hardware model, architecture, memory budget, milestones, unverified constants |
| milestone log | what each milestone verified, on which board and date, and what it did not check |
| agent instructions | build and test commands, the invariants that are easy to break, settled decisions not to reopen |

Cross-reference by section number and keep the references accurate; they are
load-bearing. Add new sections at the end of a section rather than
renumbering. **The hardware notes and the lessons are portable**: they
travel to the next project, so they may cite each other but never the design,
the milestone log or a source file. A new fact about the PicoCalc goes in the
hardware notes, written to stand alone. In code, **comments cite the document
section** next to any decision that looks arbitrary, rather than restating
the reasoning.

Keep the milestone record in its own log rather than in the agent
instructions or the design; it grows with every milestone and is read less
than either.

Use one notation for guest values and another for host values (the guest's
own convention, `#XXXX` or `$XXXX`, for guest addresses, and `0x` for host
values). A document that discusses two machines at once otherwise invites a
whole class of reading error.

### 14.2 Unverified constants

Keep a table of every guest constant written from secondary knowledge, with
its primary source and a confidence: **low, medium, high, confirmed**.
Transcribe each from the primary source before it becomes a `#define`, and
record how each was settled. A wrong constant produces a machine that boots
and then misbehaves subtly.

**The lifecycle**: while unverified, a constant is runtime configuration; once
settled, it becomes a constant and the configuration goes. The Atom's video
mode bit order and field rate both went that way, and the Ace's field shape
and INT timing.

**The best primary source is often the ROM, executed.** The keyboard matrix,
the OS entry points, the disc controller's addresses, the video byte wiring,
whether the tape reader works at 2 MHz, the key hold time, which bit carries
the tape, and whether the ROM halts or keeps a random seed were all settled
by running the original ROM on the host and observing it, not by reading a
document. The Atom's disc controller registers and video byte order, and the
Ace's cursor keys, all turned out different from what had first been
written from secondary sources.

**The other is the schematic.** One reading of the Ace's (a community
redrawing of the original board, cross-checked against a modern clone's
logic equations) settled eight rows at once: the CPU clock, the line and
field lengths, the first active line, INT's line and length, the memory and
port decode, the speaker's polarity and the wait logic. It also showed the
reference emulator's line numbering to be bitmap placement (§5.6), and the
data bus to have no pull-ups. Get it early; it is cheap to read and
expensive to do without.

**Record, do not model, what nothing depends on.** The Ace's open-bus value
and character-RAM read-back need a real machine to settle; the ROM depends
on neither, so each row records what the schematic shows and the code keeps
a configurable placeholder.

### 14.3 Milestones

Each milestone **ends with something that runs and something that is
measured**, and says when, on what board, what was verified, and what was
not. "Built" and "done" are different words: a feature is done when it has
been checked on the device. The milestone that matters is the one where the
guest boots to its prompt on the device and answers typed input; everything
before is scaffolding. Give each milestone **done-when** criteria a test or
a look at the panel can check, and a list of what it leaves out.

**Put a gate early.** The Ace's second milestone put the CPU alone on the
board and measured it against a threshold taken from the previous
emulator's shipped figure (~85 % of core 0, where the Atom ran with zero
underruns), with the levers listed in order should it fail. It passed at a
projected 23–28 %, and nothing after it had to be planned around the CPU.

A workable order, from the Atom: skeleton with both builds and CI → CPU
passing its suites on the host → bus, chips and video golden images on the
host → board bring-up with the real field loop → **guest boots** → audio →
fast media loading, snapshots, menu → perf pass → signal-level media → disc
→ remaining chip detail → recording, status, saved settings → the rest of
the menu → clock options.

The Ace's, with no disc and reused drivers:

```
 host only                          device
 M0 skeleton, both builds, CI ─────────────────────────┐
 M1 CPU on host ───────────► M2 CPU on board (GATE) ─┐ │
 M3 machine on host                M6 bring-up ◄─────┼─┘
 M4 video   M5 keyboard              │               │
   └─────────┴────────────► M7 GUEST BOOTS ON DEVICE ◄┘
 M8 audio → M9 card → M10 menu, tape trap → M11 snapshots
 M12 perf pass, soak → M13 signal tape → M14 wait states → M15 finish
```

The host-only milestones (machine, video, keyboard) ran alongside the gate
and bring-up. Sixteen milestones took three days.

Record each milestone's result where the next person will look: the date,
the board's identity, the compiler, what was checked on the device and by
whom (the owner's checks on the panel count, and say so), the shipping
build's run, and a **not checked** list. Carry each "not checked" forward
until something checks it or a decision drops it.

### 14.4 Estimates and measurements

Write the estimate first and keep it when the measurement replaces it; the
pair shows where the model was wrong. Label every remaining estimate as one.
Every measurement says what board, what build, what date and what workload.

### 14.5 Scope

**Drop features explicitly and say why** in the design. We planned and then
dropped video-bus snow (authentic, ugly, and needing a per-cycle beam
position), a scaled display, a colour-set override and a system page on the
Atom, and snow, scaling, colour themes, a faster guest clock, `.wav` tapes
and RP2040 boards on the Ace; each entry says why, so nobody re-plans it.
**Defer** rather than drop what may return (the Ace's 300 MHz clock and
snapshot export), naming what would bring it back.

### 14.6 Reusing a previous emulator

A second emulator on the same hardware should start from the first. The Ace
took its drivers (board and clocks, LCD, southbridge, keyboard ring, audio,
log ring, SD and FatFs), its frame pool, beeper, settings parser and
rewriter, status line, snapshot container, keyboard machinery, layout
parser, tools and CI from the Atom. The rules that worked:

- **Read the sibling, never edit it** from the new project.
- **Copy, rename, re-verify.** Rename the prefixes mechanically
  (`PICO_ATOM_` → `PICO_ACE_`, `atom_` → `ace_`) and change the document
  references in comments to the new project's sections.
- **Bring the tests with the code.** A copied module counts as reused only
  once its tests pass under the new names.
- **A copied file brings its attribution** entry (third-party init values,
  a filesystem's configuration).
- Sort the sibling's files into three lists in the design: **copy** (no
  guest part in them), **adapt** (structure kept, guest parts replaced:
  keyboard matrix, tape, snapshot fields, text page), and **not used**.
- **Check what moves cores.** Code that ran on core 0 in the sibling may run
  on core 1 here, and bring a `sleep_us` with it (§2.3).
- **Settle the licence first**; reuse depends on it. Same author and same
  licence made the Ace's reuse a copy.
- Expect the sibling's calibration to transfer and its estimates not to: its
  measured costs set the Ace's thresholds, and its interpreter estimate was
  wrong by 3× in the other direction (§1).

### 14.7 Decisions and reviews

**Write the owner's decisions down, numbered and dated**, with the
reasoning, and mark them settled: the default machine, shipping the ROM,
the host clock, export, the licence, which reference emulator, where an
odd-sized snapshot loads. An agent or a new contributor otherwise reopens
them. A decision that replaces an earlier one says so.

**One branch and pull request per milestone**, with CI green on both builds
before merging. An automated reviewer's pass on each was worth having: on
the Ace it found a recorder keeping partial blocks and save states not
recording the wait-state choice, among others. Answer each comment, with
the fix or the reason not to.

---

## 15. Checklist for a new emulator

Architecture

- [ ] Core/port split; core has no SDK headers and no allocation; both
      targets build under `-Werror` in CI; an SDK include in the core
      shown to fail the host build.
- [ ] Fixed capacities in one header; `arm-none-eabi-size` printed on every
      build.
- [ ] `run(n)` returns true cycles; caller carries debt; no "exactly N".
- [ ] Machine copied by function; page table rebuilt; callbacks not copied.
- [ ] Core 0: guest and audio. Core 1: LCD, I²C, SD. No `sleep_us` on core 1,
      including in reused drivers. No `printf` on core 0 once the guest runs.
- [ ] Three-buffer snapshot pool; older `ready` dropped at publish; dropped
      count on the heartbeat.
- [ ] Park/handoff at a field boundary for all card work, feeding silence;
      measuring windows restarted on resume.

CPU and devices

- [ ] 6502: functional test and a decimal test with every flag. Z80:
      ZEXDOC, ZEXALL, FUSE, and behaviour tests including Q. A skip is not a
      pass; cycle table asserted by execution; harness shown to fail.
- [ ] 6502 undocumented opcodes trapped and counted; Z80 undocumented
      behaviour implemented and `ED` holes counted.
- [ ] Page table of two pointers; flags separate; mirrors in the table;
      sub-page devices split in the slow path; I/O decoded by mask; open bus
      taken from the schematic.
- [ ] Every decoded access has its side effect; interrupt acknowledge is not
      a port read.
- [ ] Devices brought up to date on read, with a next-event countdown; run
      slices stop at device events.
- [ ] Every chip on the reset line reset with the CPU.
- [ ] Contention modelled per access if it measurably changes timing, with a
      control build.

Video

- [ ] No framebuffer if the image is a function of VRAM, mode and character
      set.
- [ ] Mode byte and character set part of the shadow; a change repaints what
      it touches; tested against a screen-only control.
- [ ] Field split at the vsync flag's or INT's edges; line numbers from the
      circuit; snapshot where the next active line begins; tested with a
      control.
- [ ] Golden images looked at before committing.
- [ ] Character ROM verified by rendering; layout asserted against the data.
- [ ] The emulator's pages in the guest's own font, from the ROM.

Audio

- [ ] Box filter at the exact rational sample period; DC blocker; silence 0.
- [ ] Guest paced on the audio queue; underruns and late refills counted
      separately; muted still consumes; the late path forced once.

Keyboard

- [ ] Held set from press/release; canonical codes; binding fixed at press.
- [ ] Matrix, hold and gap settled by executing the ROM; every entry typed
      through it in a test.
- [ ] No binding needs a chord the MCU swallows (and Shift+arrows lose their
      release); the guest's own cursor keys under guest SHIFT.

Media and settings

- [ ] ROMs by SHA-1; embedded with a failing build check where a permission
      allows, otherwise a missing-ROM page, never a blank screen.
- [ ] Traps on handlers, stalling the CPU, leaving the ROM's state byte for
      byte (or letting the ROM finish); tested against the ROM's own routine.
- [ ] Tape signal at the writer's exact counts, tested edge for edge against
      the ROM's own save; recorder keeps whole blocks.
- [ ] Snapshots: explicit fields, zero is reset, ROM hashes, timing-model
      choices recorded, two-pass load.
- [ ] Community snapshot formats settled from a description, a loader and a
      corpus; checked against a reference over the corpus.
- [ ] Every write through `.new` and rename, recovery on load.
- [ ] Settings in a card text file, edited in place, parsed back before
      writing; no flash writes.

Process

- [ ] Hardware notes and lessons portable; design and milestone log kept
      separate; cross-referenced by section.
- [ ] Unverified-constants table with sources and confidence; the schematic
      obtained early.
- [ ] A CPU-alone gate on the board before other port work.
- [ ] Every milestone ends with a device check and a measurement, dated,
      with a not-checked list.
- [ ] Reference emulator building from the first host milestone.
- [ ] Perf workloads scripted; each feature measured against a control build.
- [ ] 30-minute battery soak with every counter zero and the workload seen
      running; the release build soaked with counters read over SWD.
- [ ] Owner's decisions numbered and dated; a PR per milestone.
