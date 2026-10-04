/* card.h — the SD card's jobs and its slot (design.md §4.5, §10.1).
 *
 * Core 1 only. A job mounts the card, does its work and unmounts
 * (storage.h), so a card changed between two jobs is simply a new card.
 * A job runs only with core 0 waiting at boot or parked (park.h): card
 * latency can outlast both the field and the audio deadline
 * (hardware-notes.md §7.1).
 *
 * The slot's card detect is polled from core 1's loop, guest running or
 * not, and a change is logged and counted. It starts no card work by
 * itself: the settings are read at boot, and later jobs come from a park.
 */
#ifndef PICO_ACE_CARD_H
#define PICO_ACE_CARD_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

typedef enum {
    CARD_NONE,          /* card detect says the slot is empty      */
    CARD_UNUSABLE,      /* a card, but no FAT volume FatFs mounts  */
    CARD_MOUNTED,       /* a card, mounted for the last job        */
} card_state_t;

typedef struct {
    card_state_t state;
    int          fresult;     /* FatFs's, from the mount; 0 if mounted */
    uint32_t     mount_us;    /* sd_init and f_mount                   */
    uint32_t     read_us;     /* the settings file: open, read, parse  */
} card_job_t;

/* The boot's job: the settings file over the defaults, into *out, or the
 * defaults alone without a usable card (design.md §15.2 M9). */
void card_boot(settings_t *out, card_job_t *job);

/* A parked job: the same reading, to say what the card holds now. *out
 * is what the file says, and is not applied (M10). */
void card_check(settings_t *out, card_job_t *job);

/* The slot, debounced: true when card detect has settled at a new level
 * since the last call. Polled from core 1's loop. */
bool card_poll(void);
bool card_present(void);       /* the settled level                  */
uint32_t card_changes(void);   /* settled changes since boot         */

const char *card_state_str(card_state_t st);

#endif /* PICO_ACE_CARD_H */
