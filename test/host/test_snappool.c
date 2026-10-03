/* test_snappool.c — the frame pool (design.md §4.4, §13.2 Frame pool).
 *
 * pico-atom's pool tests under the new names, with a snapshot taken from
 * a machine: the state machine has no lock of its own, so both cores'
 * transitions can be interleaved here at random.
 */

#include <string.h>

#include "guest.h"
#include "snappool.h"
#include "test_util.h"

static snappool_t g_pool;
static guest_t g;

static unsigned rng_state = 12345;
static unsigned rng(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

int main(void) {
    snappool_t *p = &g_pool;

    /* ---- the transitions --------------------------------------------- */
    snappool_init(p);
    CHECK(snappool_take(p) < 0, "nothing to take from an empty pool");

    int a = snappool_claim(p);
    CHECK(a >= 0, "claim from an empty pool should succeed");
    CHECK(snappool_take(p) < 0, "a filling buffer is invisible to core 1");
    snappool_publish(p, a);

    int r = snappool_take(p);
    CHECK(r == a, "core 1 should take the published buffer");

    /* Core 1 is a full field behind: core 0 publishes twice while it
     * renders. The first is superseded, and core 0 never runs dry. */
    int b = snappool_claim(p);
    snappool_publish(p, b);
    int c = snappool_claim(p);
    CHECK(c >= 0, "core 0 must never run out of buffers (the third buffer)");
    snappool_publish(p, c);
    CHECK(p->dropped == 1, "the older ready snapshot should be dropped, dropped=%u",
          (unsigned)p->dropped);
    int d = snappool_claim(p);
    CHECK(d == b, "the superseded buffer should be free again for core 0");

    snappool_release(p, r);
    CHECK(snappool_take(p) == c, "core 1 should get the newest snapshot, not the oldest");

    /* Out-of-state calls change nothing. */
    snappool_publish(p, r);
    snappool_release(p, d);
    CHECK(p->state[r] == SNAP_FREE && p->state[d] == SNAP_FILLING,
          "publishing a free buffer or releasing a filling one must be ignored");

    /* ---- a long randomised interleaving ------------------------------ */
    /* Core 0 always gets a buffer, never one core 1 holds, and every
     * publish is either taken or counted as dropped. */
    snappool_init(p);
    int filling = snappool_claim(p), rendering = -1;
    uint32_t taken = 0;
    for (unsigned step = 0; step < 100000; step++) {
        if (rng() & 1u) {
            snappool_publish(p, filling);
            filling = snappool_claim(p);
            CHECK(filling >= 0 && filling != rendering,
                  "step %u: core 0 claimed %d while core 1 renders %d", step, filling,
                  rendering);
        } else if (rendering < 0) {
            rendering = snappool_take(p);
            if (rendering >= 0) taken++;
        } else {
            snappool_release(p, rendering);
            rendering = -1;
        }
        unsigned ready = 0;
        for (unsigned i = 0; i < ACE_SNAPSHOT_COUNT; i++) ready += p->state[i] == SNAP_READY;
        CHECK(ready <= 1, "step %u: %u buffers ready at once", step, ready);
    }
    unsigned pending = 0;
    for (unsigned i = 0; i < ACE_SNAPSHOT_COUNT; i++) pending += p->state[i] == SNAP_READY;
    CHECK(taken + p->dropped + pending == p->published,
          "published %u = taken %u + dropped %u + pending %u", (unsigned)p->published,
          (unsigned)taken, (unsigned)p->dropped, pending);
    printf("%u published, %u taken, %u dropped\n", (unsigned)p->published, (unsigned)taken,
           (unsigned)p->dropped);

    /* ---- a snapshot is the machine's two video inputs ---------------- */
    CHECK(guest_boot(&g, ACE_RAM_19K, 500), "boot");
    snapshot_t *s = &p->buf[0];
    snapshot_fill(s, &g.m);
    CHECK(memcmp(s->screen, ace_screen(&g.m), ACE_SCREEN_BYTES) == 0 &&
          memcmp(s->charset, ace_charset(&g.m), ACE_CHARSET_BYTES) == 0 &&
          s->field == g.m.fields,
          "snapshot_fill should copy the screen, the character set and the field");
    CHECK(memcmp(s->screen, &g.m.vram[0], ACE_SCREEN_BYTES) == 0 &&
          ace_peek(&g.m, 0x2400 + 23 * 32 + 1) == s->screen[23 * 32 + 1],
          "the snapshot's screen should be what the CPU sees at $2400");

    TEST_DONE();
}
