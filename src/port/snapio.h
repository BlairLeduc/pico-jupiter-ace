/* snapio.h — snapshots on the card: our states in /ace/states/slotN.sav
 * and the archive's .ace files in /ace/snaps/ (design.md §10.1, §10.5).
 *
 * pico-atom's snapio with the Ace's paths, and the .ace import beside it.
 * Core 1 only, with the guest parked (park.h), like all card work. A save
 * writes slotN.new, closes it, removes slotN.sav and renames the new file
 * into place: a rename alone is not proof of power-loss atomicity
 * (hardware-notes.md §7.1), so the recovery policy is on the load side. A
 * load checks slotN.sav whole — header, CRC, machine, ROM (snapshot.h) —
 * and if it is missing or damaged falls back to a whole slotN.new, which
 * is what an interrupted publish leaves. An .ace is read twice too
 * (snap_ace.h). Nothing changes in the machine until a file has passed.
 */
#ifndef PICO_ACE_SNAPIO_H
#define PICO_ACE_SNAPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "ace.h"
#include "config.h"
#include "snap_ace.h"
#include "snapshot.h"

#define SNAPIO_SLOTS     4u
#define SNAPIO_STATE_DIR "/ace/states"
#define SNAPIO_ACE_DIR   "/ace/snaps"

/* The card must be mounted (storage.h) for all of these. *us is the wall
 * time the call took. A load sets *changed when a file passed its check
 * but the second pass then failed, the card going or the file changing
 * between them: the machine is then part old, part new, and must be
 * powered on again rather than resumed. */
snap_status_t snapio_save(const ace_t *m, unsigned slot, uint32_t *us);
snap_status_t snapio_load(ace_t *m, unsigned slot, bool *recovered, bool *changed,
                          uint32_t *us);
bool          snapio_exists(unsigned slot);
bool          snapio_delete(unsigned slot);

typedef struct {
    char     path[ACE_PATH_MAX];
    uint32_t size;
} snapio_entry_t;

/* Up to max .ace files in SNAPIO_ACE_DIR, in directory order; one whose
 * path does not fit ACE_PATH_MAX is left out. */
unsigned snapio_list_ace(snapio_entry_t *out, unsigned max);

/* An .ace into m, checked first; info says what it needs. */
snap_ace_status_t snapio_load_ace(ace_t *m, const char *path, snap_ace_info_t *info,
                                  bool *changed, uint32_t *us);

#endif /* PICO_ACE_SNAPIO_H */
