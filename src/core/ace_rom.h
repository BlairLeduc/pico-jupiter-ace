/* ace_rom.h — the Ace ROM, embedded at build time (design.md §10.2).
 *
 * cmake/embed_rom.cmake writes the array from roms/ace.rom and fails the
 * build if the file's SHA-1 is not ACE_ROM_SHA1. The ROM is not under this
 * project's GPL: see roms/COPYING.md and THIRD-PARTY.md.
 */
#ifndef PICO_ACE_ACE_ROM_H
#define PICO_ACE_ACE_ROM_H

#include <stdint.h>

#include "config.h"

#define ACE_ROM_SHA1 "597ba8a15a292688333c84dc9fd35172abe5e7e6"

extern const uint8_t ace_rom[ACE_ROM_SIZE];

#endif /* PICO_ACE_ACE_ROM_H */
