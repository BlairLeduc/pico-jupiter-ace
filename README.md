# pico-jupiter-ace

A Jupiter Ace emulator for the ClockworkPi PicoCalc.

The Jupiter Ace (Jupiter Cantab, 1982) was a small British home computer that
booted into Forth rather than BASIC: a Z80A at 3.25 MHz, an 8 KiB ROM, a
32×24 character display in black and white, a 40-key keyboard, and a one-bit
speaker and cassette port. This project aims to run it on the PicoCalc, in C
against the Raspberry Pi Pico SDK.

## Status

**Early, as of 2026-10-03.** The build skeleton (M0) and the Z80 (M1) are
done: the CPU passes ZEXDOC, ZEXALL and FUSE's per-opcode tests on the
workstation. The next milestone, M2, runs the Z80 on the board to measure
whether 150 MHz is fast enough. Nothing emulates the Ace yet, and there is
no firmware worth downloading.

## What it is meant to do

- Turn the PicoCalc on and reach the Ace's Forth `OK` prompt within a second,
  at real speed, with sound.
- Type Forth on the PicoCalc's keyboard.
- Load and save programs as files on the SD card: `.tap` tapes and `.ace`
  snapshots from the Ace software archive, unmodified.
- Run as the 19K Ace (the stock machine plus a 16 KiB RAM pack), with the
  stock 3K and a 51K machine as options.
- Time every Z80 instruction, the 50 Hz interrupt and the speaker against the
  guest clock.

It needs an RP2350 board in the PicoCalc: a Pico 2, Pico 2 W or Pimoroni
Pico Plus 2 W. The RP2040 is not supported. The host clock is 150 MHz.

The first release leaves out the Ace's add-on hardware (sound boards,
printer, ROM packs), `.wav` and `.tzx` tapes, and writing `.ace` files.
[`docs/design.md`](docs/design.md) §17 gives the reason for each.

## Documentation

- [Design](docs/design.md): the Ace's hardware, the emulator's architecture,
  CPU and memory budgets, testing, the sixteen milestones, and every fact
  about the Ace that is not yet confirmed.
- [PicoCalc hardware notes](docs/hardware-notes.md): the host platform,
  its wiring, protocols, timing and quirks, as measured on real boards.
- [Emulator lessons](docs/emulator-lessons.md): what an earlier emulator for
  the same hardware taught about writing one.
- [Third-party material](THIRD-PARTY.md): what in here is not this project's,
  and under what terms.

## The ROM

The Ace's 8 KiB system ROM is in this repository, at
[`roms/ace.rom`](roms/ace.rom), and the firmware will embed it, so no ROM file
is needed on the card. It is **not covered by this project's GPL**. It is
distributed on the basis of a 1998 email from Paul Downham of Boldfield
Computing, which acquired Jupiter Cantab's remaining stock and assets, to
Edward Patel, author of the xAce emulator. The email is reproduced in
[`roms/COPYING.md`](roms/COPYING.md). Ace emulators have relied on it for over
25 years.

The build will refuse any file whose SHA-1 is not
`597ba8a15a292688333c84dc9fd35172abe5e7e6`. If you hold rights in the ROM and
object to its distribution here, please open an issue and it will be removed.

The Ace DOS ROM is not distributed: no permission covers it.

## Building

There is nothing to build yet. M0 will add a host build of the emulator core,
tested under CTest without the Pico SDK, and a `pico2` firmware build that
produces `pico-ace.uf2`. This section will say how to run both once they
exist.

## Related

pico-atom, an Acorn Atom emulator for the same hardware by the
same author, under the same licence. Its PicoCalc drivers, verified on a
Pico Plus 2 W, are the starting point for this project's.

## Licence

GPL-3.0; see [`LICENSE`](LICENSE). The Ace ROM is the exception described
above.
