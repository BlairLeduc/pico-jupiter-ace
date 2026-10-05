/* snapshot.h — our own save states, .sav (design.md §10.5; EL §8.5).
 *
 * pico-atom's format with the Z80's and the Ace's fields. Explicit,
 * field by field and little-endian, never a struct dumped from memory:
 * ace_t holds pointers and padding, and a state has to outlive the build
 * that wrote it.
 *
 *   header  20 bytes: "PACESNAP" (8), version (2), header length (2),
 *           payload length (4), CRC-32 of the payload (4)
 *   payload the state (SNAP_STATE_LEN), then RAM: the screen's 1 KiB,
 *           the character set's, user RAM's, and the expansion RAM the
 *           machine has (none, 16 KiB or 48 KiB)
 *
 * The ROM is not in a state: what is, is its SHA-1, and a state loads
 * only into a machine whose ROM hashes the same and whose RAM size,
 * field and wait states are the same, since every count in it is in that
 * machine's T-states. Unused state bytes are written zero and read as reserved, so
 * a later version can add fields whose zero is their reset value.
 *
 * States are saved between fields, where the port parks the guest, so the
 * field always resumes from its first active line; the budget carries
 * the last instruction's overshoot across, as it would have.
 *
 * I/O is through a callback, so the core never sees a file. Loading is
 * two passes: snapshot_check reads the whole stream and verifies the
 * header, the CRC and the machine without touching it; only then does
 * snapshot_load, over the same bytes again, change anything. A torn or
 * foreign file therefore leaves the running machine exactly as it was.
 */
#ifndef PICO_ACE_SNAPSHOT_H
#define PICO_ACE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ace.h"

#define SNAP_VERSION     1u
#define SNAP_HEADER_LEN  20u
#define SNAP_STATE_LEN   128u

/* The payload and file for a machine of `ram`. */
static inline uint32_t snap_payload_len(ace_ram_t ram) {
    return SNAP_STATE_LEN + 2u * ACE_BLOCK_BYTES + ace_ram_bytes(ram);
}
static inline uint32_t snap_file_len(ace_ram_t ram) {
    return SNAP_HEADER_LEN + snap_payload_len(ram);
}

typedef enum {
    SNAP_OK = 0,
    SNAP_IO,              /* the callback failed, or the file is short   */
    SNAP_NOT_SNAPSHOT,    /* wrong magic or lengths                      */
    SNAP_NEWER,           /* a version this build does not know          */
    SNAP_CORRUPT,         /* the CRC does not match                      */
    SNAP_OTHER_RAM,       /* another RAM size                            */
    SNAP_OTHER_FIELD,     /* another field shape, bus values or wait states */
    SNAP_OTHER_ROM,       /* the ROM is not the one it ran on            */
    SNAP_BUSY,            /* the CPU is stalled on a tape call           */
} snap_status_t;

/* Move exactly n bytes; false on any failure. */
typedef bool (*snap_write_fn)(void *ctx, const uint8_t *src, size_t n);
typedef bool (*snap_read_fn)(void *ctx, uint8_t *dst, size_t n);

snap_status_t snapshot_save(const ace_t *m, snap_write_fn write, void *ctx);
snap_status_t snapshot_check(const ace_t *m, snap_read_fn read, void *ctx);
snap_status_t snapshot_load(ace_t *m, snap_read_fn read, void *ctx);

const char *snapshot_status_str(snap_status_t st);

/* CRC-32 (IEEE 802.3, reflected, as zlib computes it), exposed for the
 * test that pins it against a known value. */
uint32_t snapshot_crc32(uint32_t crc, const uint8_t *p, size_t n);

#endif /* PICO_ACE_SNAPSHOT_H */
