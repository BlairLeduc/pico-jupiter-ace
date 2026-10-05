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
#define ACE_XRAM_19K        16384u  /* the 19K machine's pack, $4000-$7FFF */
#define ACE_XRAM_MAX        49152u  /* the 51K machine fills $4000-$FFFF   */

/* ---- Keyboard (design.md §2.4) --------------------------------------- */

#define ACE_KEY_ROWS            8u  /* half-rows, selected by A8-A15       */
#define ACE_KEY_COLS            5u  /* keys on D0-D4 of each               */

/* The PicoCalc's events, replayed into the matrix (design.md §9.1). */
#define ACE_KEY_EVENT_QUEUE    64u  /* southbridge FIFO holds 31 (HW §6.2) */
#define ACE_KEY_HELD_MAX        8u  /* keys down at once                   */
#define ACE_KEY_TEXT_EVENTS     4u  /* one ASCII byte: a modifier around a key */
#define ACE_KEY_RING           ACE_KEY_EVENT_QUEUE  /* core 1 to core 0 */

/* The ROM's scan takes a key on the third consecutive field that sees it
 * and needs one field with no key down before the next; it repeats a key
 * held 33 fields (ROM $0310, executed 2026-10-03, §16). The replay holds
 * and gaps one field longer than that, for a scan the guest delays. */
#define ACE_KEY_MIN_FIELDS      4u
#define ACE_KEY_GAP_FIELDS      2u

/* ---- Video (design.md §2.5, §4.4) ------------------------------------ */

#define ACE_SCREEN_COLS        32u
#define ACE_SCREEN_ROWS        24u
#define ACE_SCREEN_BYTES    (ACE_SCREEN_COLS * ACE_SCREEN_ROWS)   /* 768  */

#define ACE_CHARSET_GLYPHS    128u
#define ACE_GLYPH_ROWS          8u
#define ACE_GLYPH_COLS          8u  /* one byte a glyph row, bit 7 left    */
#define ACE_CHARSET_BYTES   (ACE_CHARSET_GLYPHS * ACE_GLYPH_ROWS) /* 1,024 */

/* The row generator's LUT (§7.2): every glyph-row byte as its pixels,
 * 256 x 8 RGB565, 4 KiB. It belongs to the presenter, not ace_t. */
#define ACE_RENDER_LUT_ENTRIES 256u

#define ACE_SNAPSHOT_COUNT      3u  /* §4.4: three, and the third is the point */

/* ---- The panel (design.md §7.4; hardware-notes.md §4) ----------------- */

#define ACE_PANEL_W           320u
#define ACE_PANEL_H           320u
#define ACE_SCREEN_W          (ACE_SCREEN_COLS * ACE_GLYPH_COLS)   /* 256 */
#define ACE_SCREEN_H          (ACE_SCREEN_ROWS * ACE_GLYPH_ROWS)   /* 192 */
#define ACE_SCREEN_X           32u  /* the guest 1:1, centred across        */
#define ACE_SCREEN_Y           64u  /* with a 64-row band above and below    */
#define ACE_LINEBUF_COUNT       2u  /* DMA ping-pong (HW §4.6)              */

/* The lines of text above and below the guest (§7.4), in the emulator's
 * font: 40 cells span the panel. The perf line sits in the middle of the
 * band below. M10: the status line in the band above. */
#define ACE_TEXT_COLS          (ACE_PANEL_W / ACE_GLYPH_COLS)      /* 40  */
#define ACE_PERF_Y             (ACE_SCREEN_Y + ACE_SCREEN_H + 28u) /* 284 */
#define ACE_LINEBUF_PIXELS  ACE_PANEL_W

/* ---- Port buffers (design.md §3.3) ------------------------------------ */

/* Core 0's log, drained by core 1 (EL §2.3): the ring, a power of two,
 * and the longest line formatted onto core 0's stack. */
#define ACE_LOG_RING         2048u
#define ACE_LOG_LINE          512u

/* ---- The card (design.md §10) ----------------------------------------- */

#define ACE_PATH_MAX          128u  /* a path on the card, with its NUL    */
#define ACE_SETTINGS_FILE_MAX 2048u /* /ace/pico-ace.cfg, read whole       */
#define ACE_SETTINGS_LINE_MAX (ACE_PATH_MAX + 32u)  /* "boot_tape = " and a path */
#define ACE_KEYMAP_NAME_LEN    16u  /* a layout's name (§9.4)              */
#define ACE_TAPE_CHUNK        512u  /* card reads and writes for the tape  */
#define ACE_TAPE_IMAGE_MAX  65536u  /* a .tap whole, for the signal (§3.3, §10.4) */
#define ACE_TAPE_LIST_MAX      64u  /* .tap files the Tape page lists      */
#define ACE_SNAP_LIST_MAX      64u  /* .ace files the Snapshot page lists  */

/* ---- Timing (design.md §2.1, §11) ------------------------------------ */

#define ACE_CPU_HZ        3250000u  /* 6.5 MHz crystal / 2                 */

/* VIDEN, the part of a display line in which the video circuit fetches
 * and the waiting mirrors hold the CPU: 256 pixels at 6.5 MHz (§6.4). */
#define ACE_VIDEN_T          (ACE_SCREEN_W / 2u)                  /* 128 T */

/* The rest of the field's shape (§11.1) is runtime configuration in
 * ace_config_t until §16 settles it. */

/* ---- Audio (design.md §8; EL §6; hardware-notes.md §5) ---------------- */

#define ACE_PWM_TOP          2047u  /* 11 bits, 73.2 kHz carrier          */
#define ACE_PWM_OVERSAMPLE      2u  /* each frame written twice           */

/* The nominal sample rate as a fraction, 150,000,000 / 4,096 Hz, which
 * makes a sample 6,656/75 T (§8). The port recomputes it from
 * clock_get_hz(clk_sys) and hands the real one to ace_audio_set_rate;
 * this is what a host build runs at. */
#define ACE_AUDIO_RATE_NUM  150000000u
#define ACE_AUDIO_RATE_DEN  ((ACE_PWM_TOP + 1u) * ACE_PWM_OVERSAMPLE)

/* Samples the core holds between drains. One field is 731.25; the port
 * drains after every field (§8). */
#define ACE_AUDIO_BUF_LEN    1024u

#define ACE_PCM_QUEUE_LEN    1024u  /* SPSC, ~28 ms (EL §6.2)             */
#define ACE_PCM_QUEUE_START   768u  /* start streaming at this depth      */

/* Power-of-two AND aligned, with the hardware read wrap (HW §5.3). At
 * oversample 2 a frame is two slots; a half is 3.5 ms of sound. */
#define ACE_DMA_FRAMES_PER_HALF 128u
#define ACE_DMA_SLOTS_PER_HALF  (ACE_DMA_FRAMES_PER_HALF * ACE_PWM_OVERSAMPLE)
#define ACE_DMA_RING_SLOTS      (ACE_DMA_SLOTS_PER_HALF * 2u)

#endif /* PICO_ACE_CONFIG_H */
