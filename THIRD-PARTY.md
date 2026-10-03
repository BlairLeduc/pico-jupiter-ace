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
(`docs/design.md` §10.2). If you hold rights in the ROM and object to its
distribution here, please open an issue and it will be removed.

## font8x8

`third_party/font8x8/` holds `font8x8_basic.h` and `README`, unmodified, from
Daniel Hepper's [font8x8](https://github.com/dhepper/font8x8) at `8e279d2`,
fetched 2026-10-03. It is **public domain**, as its header and README state:
derived from Marcel Sondaar's `font8_8.asm`, after IBM's public-domain VGA
fonts. `tools/mkfont.py` writes `src/core/font.c` from it with each byte's
bits reversed, the emulator's own font for its pages (`docs/design.md` §7.5).
