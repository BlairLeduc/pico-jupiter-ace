/* card.c — the SD card's jobs and its slot (card.h, design.md §10.1). */

#include "card.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "log.h"
#include "sd.h"
#include "settingsio.h"
#include "storage.h"
#include "tapeio.h"

/* Card detect must hold a new level this long before it counts: the
 * contacts bounce as a card goes in, and a card half in does not answer
 * (hardware-notes.md §7.1). */
#define CARD_SETTLE_US 250000u

static volatile bool s_present;   /* settled; core 0 reads it */
static bool     s_raw;            /* last read */
static uint32_t s_raw_since;
static volatile uint32_t s_changes;
static bool     s_polled;

/* boot_tape: a bare name is a file in /ace/tapes/ (design.md §10.6). A
 * build-time PICO_ACE_BOOT_TAPE wins over the file's (EL §8.7). */
static void boot_tape(const settings_t *s) {
    const char *name = s->boot_tape;
#ifdef PICO_ACE_BOOT_TAPE
    name = PICO_ACE_BOOT_TAPE;
#endif
    if (!name[0]) return;
    char path[ACE_PATH_MAX + sizeof SETTINGS_TAPE_DIR];
    if (strchr(name, '/')) snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "%s/%s", SETTINGS_TAPE_DIR, name);
    const char *err = tapeio_insert(NULL, path);
    log_core1("  card         : boot_tape %s%s%s\n", path, err ? ": " : " in the deck",
              err ? err : "");
}

static void job(settings_t *out, card_job_t *j, const char *why) {
    j->mount_us = j->read_us = 0;
    j->fresult = 0;
    if (!sd_present()) {
        j->state = CARD_NONE;
        settingsio_none(out);
        log_core1("  card         : %s: no card\n", why);
        return;
    }

    uint32_t t0 = time_us_32();
    j->fresult = storage_mount();
    j->mount_us = time_us_32() - t0;
    if (j->fresult != 0) {
        j->state = CARD_UNUSABLE;
        settingsio_none(out);
        log_core1("  card         : %s: no FAT volume (FatFs %d) after %lu us\n", why,
               j->fresult, (unsigned long)j->mount_us);
        return;
    }
    j->state = CARD_MOUNTED;

    t0 = time_us_32();
    settingsio_load(out);
    j->read_us = time_us_32() - t0;
    if (why[0] == 'b') boot_tape(out);
    storage_unmount();
    log_core1("  card         : %s: mounted in %lu us, settings %s in %lu us\n", why,
           (unsigned long)j->mount_us, settingsio_state_str(settingsio_state()),
           (unsigned long)j->read_us);
}

void card_boot(settings_t *out, card_job_t *j) {
    job(out, j, "boot");
    s_polled = true;
    s_present = s_raw = sd_present();
    s_raw_since = time_us_32();
}

void card_check(settings_t *out, card_job_t *j) {
    job(out, j, "parked");
}

bool card_poll(void) {
    uint32_t now = time_us_32();
    if (!s_polled) {
        s_polled = true;
        s_present = s_raw = sd_present();
        s_raw_since = now;
        return false;
    }
    bool raw = sd_present();
    if (raw != s_raw) {
        s_raw = raw;
        s_raw_since = now;
        return false;
    }
    if (raw == s_present || now - s_raw_since < CARD_SETTLE_US) return false;
    s_present = raw;
    s_changes++;
    return true;
}

bool card_present(void) {
    return s_present;
}

uint32_t card_changes(void) {
    return s_changes;
}

const char *card_state_str(card_state_t st) {
    switch (st) {
    case CARD_NONE:     return "none";
    case CARD_UNUSABLE: return "unusable";
    case CARD_MOUNTED:  return "mounted";
    }
    return "?";
}
