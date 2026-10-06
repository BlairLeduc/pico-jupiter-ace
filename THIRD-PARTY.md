# Third-party material

Everything in this repository is this project's own work under the GPL-3.0 in
[`LICENSE`](LICENSE), with the exceptions below.

## Jupiter Ace ROM

`roms/ace.rom` is the Jupiter Ace's 8 KiB system ROM, made by Jupiter Cantab
(1982), the company of Richard Altwasser and Steven Vickers. It is **not covered
by this project's GPL**. It is distributed on the basis of the permission
recorded in [`roms/COPYING.md`](roms/COPYING.md): a 1998 email from Paul
Downham of Boldfield Computing, which acquired Jupiter Cantab's remaining
stock and assets, to Edward Patel, author of the xAce emulator.

| | |
|---|---|
| Size | 8,192 bytes |
| SHA-1 | `597ba8a15a292688333c84dc9fd35172abe5e7e6` |

The firmware embeds the ROM unmodified, and the build refuses any other file
(`docs/design.md` §10.2). **If you hold rights in the ROM and object to its
distribution here, please open an issue and it will be removed.**

## Jupiter Ace schematic — Bodo Wenzel, commented by nocash

`docs/ace-sch-nocash.gif` is a schematic of the Jupiter Ace's original
board (2114 RAMs), drawn by Bodo Wenzel (dated March 16, 2006 on the
drawing) and commented and rearranged by Martin Korth (nocash) in July
2010, as its title block says. It is **not covered by this project's GPL**
and is kept unmodified, as a reference: `docs/design.md` §6.4 and §16
settle the wait logic, the field's timing, the port and the memory
decode from it. No licence is stated on the drawing. If you hold rights
in it and object to its distribution here, please open an issue and it
will be removed.

## PicoCalc LCD initialisation values — ClockworkPi

`src/port/lcd.c`, copied from pico-atom, sends the panel's gamma, power, VCOM,
frame-rate, inversion, display-function and manufacturer commands with the
parameter bytes used by ClockworkPi's own driver,
`Code/picocalc_helloworld/lcdspi/lcdspi.c` in
<https://github.com/clockworkpi/PicoCalc> at commit
`f91519806d4b2e0a62c4638a9f695cd5162c5479`. Only the register values were taken
— the driver around them is this project's — and hardware-notes.md §4.4 directs
using them because generic controller defaults may not suit this glass. The
pixel format (`0x55`) and entry mode (`0x06`) differ from that driver's 18-bit
setup and come from hardware-notes.md §4.4 instead.

GitHub detects no licence file in that repository at that revision. The
southbridge reply layout in `src/port/southbridge.c` was likewise checked
against the same repository's keyboard firmware, but no code was taken from it.


## FatFs — ChaN

The firmware links FatFs R0.15 (with patch 1) for the SD card, as pico-atom
does, from whose `src/port/` the card files were copied. It is **not in
the tree**: CMake copies `ff.c`, `ff.h`, `ffunicode.c` and `diskio.h` out of
the Pico SDK's `lib/tinyusb/lib/fatfs/source/` into the build directory at
configure time. The one FatFs file in the repository is
`src/port/fatfs/ffconf.h`, which started as the SDK's copy and records in its
header which values were changed.

> FatFs — Generic FAT Filesystem Module
> Copyright (C) 2022, ChaN, all right reserved.
> <http://elm-chan.org/fsw/ff/>

FatFs's licence, from the header of `ff.c`, is reproduced in full:

```
Copyright (C) 2022, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in
source and binary forms, with or without modification, are permitted provided
that the following condition is met:

1. Redistributions of source code must retain the above copyright notice,
   this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS"
and any warranties related to this software are DISCLAIMED.
The copyright owner or contributors be NOT LIABLE for any damages caused
by use of this software.
```

Its one condition applies to source, and `ffconf.h` carries the notice for
that reason. Binary redistribution has no condition in this version of the
licence; the notice is reproduced here anyway.

