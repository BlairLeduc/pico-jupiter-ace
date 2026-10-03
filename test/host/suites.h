/* suites.h — where the fetched test suites are (design.md §5.4).
 *
 * tools/fetch-test-suites.sh puts them in test/suites, which is what
 * PICO_ACE_DEFAULT_TEST_SUITES names; PICO_ACE_TEST_SUITES in the
 * environment wins. A test whose file is missing exits TEST_SKIP_CODE:
 * reported as skipped, never as passed.
 */
#ifndef PICO_ACE_SUITES_H
#define PICO_ACE_SUITES_H

#include <stdio.h>
#include <stdlib.h>

#include "test_util.h"

/* Opens name in the suites directory, or exits as skipped. */
static inline FILE *suite_open(const char *name, const char *mode) {
    const char *dir = getenv("PICO_ACE_TEST_SUITES");
    if (!dir || !*dir)
        dir = PICO_ACE_DEFAULT_TEST_SUITES;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, mode);
    if (!f) {
        printf("SKIP: %s not found; run tools/fetch-test-suites.sh\n", path);
        exit(TEST_SKIP_CODE);
    }
    return f;
}

#endif /* PICO_ACE_SUITES_H */
