# pico-jupiter-ace

A Jupiter Ace emulator for the ClockworkPi PicoCalc, on a Raspberry Pi
Pico 2 or compatible board. Unfortunately, a board based on the RP2040
(Pico or the like) is not supported as it is too slow to run the Z80
in real time.

The Jupiter Ace (Jupiter Cantab, 1982) is a small British home computer
that runs Forth instead of BASIC. It has a Z80A at 3.25 MHz, an 8 KiB ROM
holding the Forth system, a 32×24 character screen whose characters can
all be redefined, a 40-key keyboard, and a one-bit speaker. This emulator
runs the original ROM on an emulated Z80, in real time, with sound, tape
and snapshots.

> [!NOTE]
> It does not emulate the Ace's add-on hardware (sound boards, printer, ROM
> packs), `.wav` and `.tzx` tapes, and writing `.ace` files;
> [`docs/design.md`](docs/design.md) §17 provides the reason for each.

## Documentation

- [Design](docs/design.md): the Ace as emulated, the architecture,
  budgets, milestones, and every guest fact and how each was settled.
- [PicoCalc hardware notes](docs/hardware-notes.md): the host platform.
- [Emulator lessons](docs/emulator-lessons.md): what an earlier emulator
  on the same hardware taught.
- [Third-party material](THIRD-PARTY.md): what in here is not ours, and
  under what terms.

## The ROM

The Ace's ROM is in this repository, at `roms/ace.rom`, and is built into
the firmware, so the emulator needs nothing on the card to start. It is
**not covered by this project's GPL**.

The ROM was written by Jupiter Cantab, the company of Richard Altwasser and
Steven Vickers. When Jupiter Cantab went into liquidation, Boldfield
Computing bought its remaining stock and assets. In 1998 Paul Downham of
Boldfield wrote to Edward Patel, the author of the xAce emulator: "I seem
to remember that we gave the ROM listing away, but I am sure nobody is
going to get upset about the emulator". The whole email is in
[`roms/COPYING.md`](roms/COPYING.md). It is informal, and it does not show
that the ROM's copyright passed to Boldfield, but it is what Ace emulators
have relied on for more than 25 years. If you hold rights in the ROM and
object to its distribution here, please open an issue and it will be
removed.

The build refuses any file but this one:

```
597ba8a15a292688333c84dc9fd35172abe5e7e6  roms/ace.rom
```

The menu's *About* page shows the same SHA-1, read from the firmware.

## Building and flashing

You need the Raspberry Pi Pico SDK (2.x) and `arm-none-eabi-gcc` on your
`PATH`, with `PICO_SDK_PATH` set:

```sh
tools/build.sh -DPICO_ACE_UART=OFF build/pico-release
```

This makes `build/pico-release/pico-ace.uf2`. Hold BOOTSEL on the Pico
while connecting it to a computer, and copy the `.uf2` onto the drive
that appears. One image runs on all three boards.

## The SD card

The card is optional. Without one the Ace runs with its defaults, and has
no tapes, snapshots or saved states. Everything the emulator reads or
writes is under `/ace/`:

```
/ace/
  pico-ace.cfg          the settings, read at power-on (below)
  tapes/*.tap           tape images
  snaps/*.ace           snapshots in the format other Ace emulators use
  states/slot1.sav      the emulator's own saved states, slots 1 to 4
  keymaps/*.map         keyboard mapping layouts (below)
  shots/SHOT0001.bmp    screenshots, numbered in order
```

> [!WARNING]
> The card must be formatted as FAT32, or FAT16 for small cards.

## Software

The largest collection is the **Jupiter Ace Archive**,
<https://www.jupiter-ace.co.uk/>, with programs as `.tap` and `.ace`
files, scanned magazines, and the Ace's manuals. Put `.tap` files in
`/ace/tapes/` and `.ace` files in `/ace/snaps/`. The TOSEC collection's
Jupiter Ace set holds much the same files.

## Using it

The PicoCalc boots straight to the Ace's prompt: a blank screen with a
cursor at the bottom. The Ace prints `OK` only after a line has run.
Type `2 2 + .` and Enter, and it prints `4  OK`. `VLIST` lists every word
it knows.

### Keys

The PicoCalc's keys type what is printed on them, and the emulator
presses the Ace keys that give the same character. The ones that differ:

| On the Ace | On the PicoCalc |
|---|---|
| `SHIFT` | `Shift` |
| `SYMBOL SHIFT` | `Ctrl`, held with a key |
| `DELETE` | `Backspace` or `Del` |
| the cursor, left, right, up and down | the arrow keys |
| `BREAK` (stops a running word) | `Esc`, or `Shift`+`Esc` |
| `CAPS LOCK` | `Alt`+`L` |
| `GRAPHICS` | `Alt`+`G` |
| `INVERSE VIDEO` (on, then off) | `Alt`+`V` |
| `DELETE LINE` | `Alt`+`X` |
| `£` | `` ` `` |
| `©` | `Ctrl`+`I` |

Use the following key-bindings to access the emulator itself:

| | |
|---|---|
| the emulator's menu | `Alt`+`M` |
| specific menu page, then back to the Ace | `F1` Tapes, `F3` Snapshots, `F4` Setup, `F5` Machine |
| help, shows these key bindings | `Alt`+`H` |
| the About page | `F10` |
| screenshot of whatever is on the display, to the card | `F6` |
| pause | `Alt`+`P` |
| reset the Ace, keeping its memory | `Alt`+`K` |

A page opened with a function key or `Alt`+`H` goes back to the Ace when
you leave it. In the menu the arrows move, `Enter` chooses, left and
right change a value, and `Esc` goes back a page, or to the Ace from the
main page. The row at the bottom names the first problem the emulator
has found, if there is one, and the title row shows the battery's charge.

**Pause.** `Alt`+`P` stops the Ace where it is and dims the screen. The
bottom line says `Paused`. Any key carries on, and that key is not
typed. `Alt`+`M`, `Alt`+`H` and the function keys open the menu instead.

**The lines above and below the screen.** The line along the bottom, the
status line, shows the tape in the deck: with fast tape on, its name;
with fast tape off, whether it is stopped, playing or recording, how far
through it is, and the speed while it runs fast. The line along the top,
the perf line, shows the emulator's own figures: how much of the
PicoCalc's first core the Ace takes, how many times real time it could
run, the slowest screen update in the last second, frames dropped, and
sound underruns and late refills since power-on. The menu's *Setup* page
turns each on or off; the status line starts on and the perf line off.

### Tapes

Tapes are `.tap` files in `/ace/tapes/`. `LOAD NAME` loads `NAME.tap`, or
failing that the first tape whose first file is called `NAME`. `SAVE NAME`
writes `NAME.tap`, adding to it if it is there. `BLOAD`, `BSAVE` and
`VERIFY` work the same way. To use a tape whose file names do not match
the program's, put it in the deck on the menu's *Tapes* page; the page
shows each tape's first file. With a tape in the deck, a `LOAD` reads it
from where it stands, as a cassette would, unless the card has a file of
the name asked for, and a `SAVE` adds to the end of it. *Rewind* takes it
back to the start, and *Eject* goes back to finding files by name. To
start a blank tape, choose *New tape*: it makes `TAPE01.tap` (or the next
free number) and puts it in the deck, and every `SAVE` adds to it.

**Fast tape** is on unless you turn it off on the *Setup* page. With it
on, a load or save takes a fraction of a second. With it off, the Ace
reads and writes the tape's signal, as a real one does. A tape then takes
as long as it did in 1982, divided by about three, since the emulator runs
the Ace as fast as it can while a tape plays, without sound. 

> [!TIP]
A few programs load through a loader of their own and need fast tape off and
*Play* on the *Tapes* page.

### Snapshots and saved states

The *Snapshots* page loads `.ace` files from `/ace/snaps/`, the snapshot
format of the Ace's other emulators. A file saved on a 35K Ace loads into
the 51K. A file for another machine is refused, and the page names the
machine it needs; change it on the *Machine* page first.

The same page saves the whole machine to one of four slots in
`/ace/states/`, and loads it back. 

> [!IMPORTANT]
> A saved state is stored using this emulator's own format, and can only
> be loaded using the same machine configuration that saved it.

### The machine

The Ace was sold with 1 KiB of RAM for programs, and is called the 3K
after its total memory. RAM packs made it the 19K and the 51K. The
emulator starts as the **19K**, which most programs need. The *Machine*
page changes it: left and right choose, and *Apply and restart* turns the
Ace off and on again as the new machine. The program in memory is lost,
so save it first. The tape stays in the deck. To start as another machine
every time, choose *Save settings* afterwards.

### Game layouts

Ace games read the keyboard themselves, and many move on keys that are
not in good placement for the PicoCalc. A game layout adjusts the standard
map, and every other key types as before. The *Keys* row of the *Setup*
page (`F4`) chooses one, as pico-atom's does.

Two are built in:

| Layout | `Left` | `Up` | `Down` | `Right` | `]` |
|---|---|---|---|---|---|
| *CURSOR* | `5` | `6` | `7` | `8` | `0` |
| *QAOP* | `O` | `Q` | `A` | `P` | `SPACE` |

*CURSOR* is for games that move on the keys the Ace's own cursor arrows
are on, and *QAOP* for games that use those letters. Fire is `]`, away
from the arrows, because the PicoCalc's keyboard sends nothing for the
arrows while `Shift` is held.

Further layouts are text files in `/ace/keymaps/`, one key a line:

```
# Invaders: move on the arrows, fire on the space bar
name  = INVADERS
left  = Z
right = X
space = M
tapes = invaders, invade2
```

On the left is a PicoCalc key: `left`, `right`, `up`, `down`, `space`,
`enter`, `backspace`, `tab`, `del`, `esc`, or any single character,
shifted or not. On the right is an Ace key: `A`–`Z`, `0`–`9`, `SPACE`,
`ENTER`, `SHIFT` or `SYMBOL`. The Ace sees the key alone, without the
`SHIFT` the standard map would add, because a game reading the keyboard
looks for the key. `name` is what the menu shows, at most 16 characters,
and must differ from every other layout's; without it the file's own name
is used. The optional `tapes` line names up to four files, without their
`.tap` or `.ace`: putting one of them in the deck, or loading it on the
*Snapshots* page, chooses the layout, and the menu says so. A
line starting with `#` is a comment, except `# = ...`, which binds the `#`
key. A file that does not parse is left out, and the menu names the file
and line.

### Settings

At power-on the emulator reads `/ace/pico-ace.cfg`, if the card has one.
Each line sets one thing, and anything the file leaves out keeps its
default. This file sets everything to its default:

```
# /ace/pico-ace.cfg
ram       = 19k        # 3k, 19k or 51k
volume    = 8          # 0-8
layout    = standard   # or a layout's name, as the Setup page shows it
boot_tape =            # a tape in /ace/tapes/, in the deck at power-on
perf_line = off        # the emulator's own figures along the top; or on
status    = on         # the tape along the bottom; or off
backlight = 8          # 1-15, as the menu shows it; leave out to keep the last
fast_tape = on         # off: the Ace reads and writes the tape's signal
```

> [!NOTE]
> The backlight is the one exception: it is left alone unless the file
> sets it.

Upper and lower case are the same. A `#` at the start of a line, or after
a space, starts a comment. A line that is wrong is skipped and the rest
are used, and so is a tape or layout the card does not have; the menu's
bottom row and the *About* page name the first problem.

*Save settings* on the menu's first page writes what is in force: the
machine as it is running, the volume, the backlight, the status and perf
lines, fast tape, the layout you chose, and the tape you put in the deck. It edits the file
rather than replacing it. A line it changes keeps its place and its
comment, and a setting the file does not mention is added at the end,
only if it differs from its default. Anything else is left as it is. A
layout chosen by loading a file is not saved, since you did not choose
it. With no file on the card, the save makes one.

### About

The *About* page (`F10`), shows the firmware's version, the board it was
built for, the chip, its revision and clock, the keyboard controller's
version, the chip's temperature, the machine, the ROM with the first
eight digits of its SHA-1, to check against the one above, and the state
of the settings file.

## Books

**Jupiter Ace FORTH Programming**, Steven Vickers, Jupiter Cantab, 1982,
is the manual that came with the Ace. It teaches Forth from the first
line and covers the Ace's sound, graphics, tape and hardware. The Jupiter
Ace Archive has it, with the Ace's other books and the magazines that
covered it.

## Related

pico-atom, an Acorn Atom emulator for the same hardware by the same
author, under the same licence. Its PicoCalc drivers were the starting
point for this project's.

## Licence

GPL-3.0, in [`LICENSE`](LICENSE), except the material
[`THIRD-PARTY.md`](THIRD-PARTY.md) lists, the ROM among it.
