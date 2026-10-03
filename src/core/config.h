/* config.h — every fixed capacity in the emulator, in one place.
 *
 * Nothing in src/core/ allocates; every buffer is sized from a constant
 * here, so the SRAM budget (design.md §3.3) is a link-time fact.
 *
 * A guest fact becomes a #define only once it is settled (design.md §16).
 * The ones below are this project's own choices or rated high there; the
 * mirrors, the field's timing and the rest arrive as M3 settles them.
 *
 * Guest addresses are written $XXXX in comments and 0x in code.
 */
#ifndef PICO_ACE_CONFIG_H
#define PICO_ACE_CONFIG_H

/* ---- Guest address space (design.md §2.2, §6.1) ---------------------- */

#define ACE_ADDR_SPACE      65536u
#define ACE_PAGE_SIZE         256u  /* page_t {read, write} per page      */
#define ACE_PAGE_COUNT      (ACE_ADDR_SPACE / ACE_PAGE_SIZE)

#define ACE_ROM_BASE       0x0000u  /* $0000-$1FFF                         */
#define ACE_ROM_SIZE         8192u  /* roms/ace.rom (§10.2)                */

/* Video, character and user RAM are 1 KiB each, decoded in 4 KiB windows
 * and so mirrored (§2.2). The ROM writes the screen at $2400, its
 * workspace at $2700 and the character set at $2C00, and keeps its
 * system variables at $3C00 (read from the ROM, 2026-10-03). */
#define ACE_BLOCK_BYTES      1024u
#define ACE_VRAM_BASE      0x2000u  /* $2000-$27FF: 1 KiB twice            */
#define ACE_CRAM_BASE      0x2800u  /* $2800-$2FFF: 1 KiB twice, write-only */
#define ACE_URAM_BASE      0x3000u  /* $3000-$3FFF: 1 KiB four times       */
#define ACE_XRAM_BASE      0x4000u  /* expansion, $4000 up (§6.2)          */
#define ACE_XRAM_MAX        49152u  /* the 51K machine fills $4000-$FFFF   */

/* ---- Video (design.md §2.5, §4.4) ------------------------------------ */

#define ACE_SCREEN_COLS        32u
#define ACE_SCREEN_ROWS        24u
#define ACE_SCREEN_BYTES    (ACE_SCREEN_COLS * ACE_SCREEN_ROWS)   /* 768  */

#define ACE_CHARSET_GLYPHS    128u
#define ACE_GLYPH_ROWS          8u
#define ACE_CHARSET_BYTES   (ACE_CHARSET_GLYPHS * ACE_GLYPH_ROWS) /* 1,024 */

#define ACE_SNAPSHOT_COUNT      3u  /* §4.4: three, and the third is the point */

/* ---- Timing (design.md §2.1, §11) ------------------------------------ */

#define ACE_CPU_HZ        3250000u  /* 6.5 MHz crystal / 2                 */

/* The rest of the field's shape (§11.1) is runtime configuration in
 * ace_config_t until §16 settles it. */

#endif /* PICO_ACE_CONFIG_H */
