/* test_skeleton.c — M0 (design.md §15.2). Compiles the core's headers
 * with the system compiler and no Pico SDK, so an SDK #include in one of
 * them fails the host build, and checks the committed ROM against the
 * size config.h gives it. */

#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "hot.h"
#include "test_util.h"

/* A host build is tier 0: the macro must leave an ordinary function. */
static int ACE_HOT2(hot_identity)(int x) { return x; }

int main(void) {
    CHECK(hot_identity(42) == 42, "ACE_HOT2 changed the function");

    CHECK(ACE_PAGE_COUNT * ACE_PAGE_SIZE == ACE_ADDR_SPACE,
          "%u pages of %u bytes", ACE_PAGE_COUNT, ACE_PAGE_SIZE);
    CHECK(ACE_ROM_SIZE % ACE_PAGE_SIZE == 0,
          "the ROM does not fill whole pages of the page table (§6.1)");

    /* The tests that run the ROM never skip for want of it (§10.2). */
    FILE *f = fopen(PICO_ACE_ROM_PATH, "rb");
    CHECK(f != NULL, "cannot open %s", PICO_ACE_ROM_PATH);
    if (f) {
        static uint8_t buf[ACE_ROM_SIZE + 1];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        CHECK(n == ACE_ROM_SIZE, "%s is %zu bytes, not %u", PICO_ACE_ROM_PATH, n,
              ACE_ROM_SIZE);
    }

    TEST_DONE();
}
