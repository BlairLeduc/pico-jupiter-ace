/* snappool.h — the core 0 -> core 1 video handoff (design.md §4.4).
 *
 * Three snapshot buffers, each in one of four states. Core 0 fills one
 * per field and publishes it; core 1 takes the newest published one,
 * renders it, and releases it. Core 1 never reads guest RAM while the
 * Z80 runs — it only ever sees a snapshot it owns.
 *
 * This file is the state machine and nothing else. It holds no lock:
 * RP2350 has no compare-and-swap and local interrupt masking is not a
 * multicore lock (hardware-notes.md §9.5), so the port calls every
 * transition here under one SIO spinlock. Keeping the lock out is what
 * lets the transitions be tested on a host (EL §2.4).
 */
#ifndef PICO_ACE_SNAPPOOL_H
#define PICO_ACE_SNAPPOOL_H

#include <stdbool.h>
#include <stdint.h>

#include "ace.h"
#include "config.h"

typedef enum {
    SNAP_FREE,       /* owned by nobody; core 0 may claim it        */
    SNAP_FILLING,    /* core 0 is writing it; core 1 must not look  */
    SNAP_READY,      /* complete, awaiting collection               */
    SNAP_RENDERING,  /* core 1 owns it                              */
} snap_state_t;

/* What core 1 needs from a field (§4.4): the two video inputs, and the
 * field number for the perf line. */
typedef struct {
    uint8_t  screen[ACE_SCREEN_BYTES];
    uint8_t  charset[ACE_CHARSET_BYTES];
    uint32_t field;
    /* M10: tape position and the status line's flags (§4.4). */
} snapshot_t;

typedef struct {
    snapshot_t   buf[ACE_SNAPSHOT_COUNT];
    snap_state_t state[ACE_SNAPSHOT_COUNT];
    uint32_t     published;   /* snapshots published                */
    uint32_t     dropped;     /* superseded before core 1 took them */
} snappool_t;

void snappool_init(snappool_t *p);

/* Core 0: take a free buffer to fill. Returns its index, or -1 if none
 * is free — which three buffers make impossible while each side holds
 * at most one, so -1 is a bug, not back-pressure. */
int  snappool_claim(snappool_t *p);

/* Core 0: filling -> ready. Any older ready buffer is superseded and
 * freed on the spot, unrendered, so there is always a free buffer for the
 * next claim and core 0 never has to wait for core 1. */
void snappool_publish(snappool_t *p, int i);

/* Core 1: take the ready buffer, if there is one; ready -> rendering.
 * Returns its index or -1. */
int  snappool_take(snappool_t *p);

/* Core 1: rendering -> free. */
void snappool_release(snappool_t *p, int i);

/* Core 0, at a field's end, into the buffer it claimed: the screen
 * ($2400) and the character set ($2C00), copied from the machine. */
void snapshot_fill(snapshot_t *s, const ace_t *m);

#endif /* PICO_ACE_SNAPPOOL_H */
