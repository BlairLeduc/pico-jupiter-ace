/* hot.h — which of the core's functions run from SRAM (design.md §3.2,
 * hardware-notes.md §9.2, §9.8).
 *
 * On the device, flash code is fetched through a 16 KiB XIP cache that
 * both cores share, and an interpreter alone is larger than that. The
 * SDK's linker script copies every `.time_critical*` input section into
 * SRAM at boot, so naming a function's section is all it takes to move
 * it. That is the SDK's convention, not an SDK dependency: nothing here
 * includes a Pico header.
 *
 * Moves come in tiers, and PICO_ACE_RAM_TIER says how many are taken.
 * None is measured on this project yet: M2 measures the first against
 * flash, and M12 chooses the one that ships (design.md §15.2). A host
 * build leaves it at 0 and gets ordinary functions.
 *
 *   ACE_HOT1(name)  what the interpreter calls out to: the bus slow path,
 *                   port I/O, the beeper.
 *   ACE_HOT2(name)  the interpreter and its loop.
 *
 * Whole files are the wrong unit (hardware-notes.md §9.2): mark the
 * function, not the module, and check the symbol moved from 0x1... to
 * 0x2... with arm-none-eabi-nm after the build (hardware-notes.md §9.8).
 */
#ifndef PICO_ACE_HOT_H
#define PICO_ACE_HOT_H

#ifndef PICO_ACE_RAM_TIER
#define PICO_ACE_RAM_TIER 0
#endif

#define ACE_IN_RAM_(name) __attribute__((section(".time_critical.ace_" #name))) name

#if PICO_ACE_RAM_TIER >= 1
#define ACE_HOT1(name) ACE_IN_RAM_(name)
#else
#define ACE_HOT1(name) name
#endif

#if PICO_ACE_RAM_TIER >= 2
#define ACE_HOT2(name) ACE_IN_RAM_(name)
#else
#define ACE_HOT2(name) name
#endif

#endif /* PICO_ACE_HOT_H */
