/* test_rom_embed.c — the build embeds roms/ace.rom unchanged and refuses
 * a corrupted copy (design.md §10.2, §15.2 M3).
 *
 * Runs cmake/embed_rom.cmake as the build does, on the real file and on a
 * copy with one byte changed. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ace_rom.h"
#include "test_util.h"

static uint8_t rom[ACE_ROM_SIZE + 1];

/* Exit status of the embed script on `rom_path`, and whether it wrote
 * its output. */
static int embed(const char *rom_path, const char *out, int *wrote) {
    remove(out);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" -DROM=\"%s\" -DSHA1=%s -DOUT=\"%s\" -P \"%s\" >/dev/null 2>&1",
             PICO_ACE_CMAKE, rom_path, ACE_ROM_SHA1, out, PICO_ACE_EMBED_ROM);
    int status = system(cmd);
    FILE *f = fopen(out, "rb");
    *wrote = f != NULL;
    if (f) fclose(f);
    return status;
}

int main(void) {
    FILE *f = fopen(PICO_ACE_ROM_PATH, "rb");
    CHECK(f != NULL, "cannot open %s", PICO_ACE_ROM_PATH);
    if (!f) TEST_DONE();
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    CHECK(n == ACE_ROM_SIZE, "%s is %zu bytes", PICO_ACE_ROM_PATH, n);

    /* What the core links is the file, byte for byte. */
    CHECK(memcmp(rom, ace_rom, ACE_ROM_SIZE) == 0, "ace_rom differs from %s", PICO_ACE_ROM_PATH);

    const char *out = "test_rom_embed.out.c";
    int wrote;

    /* The control: the real file embeds. */
    CHECK(embed(PICO_ACE_ROM_PATH, out, &wrote) == 0 && wrote, "the real ROM was refused");

    /* One byte changed is refused, and nothing is written. */
    const char *bad = "test_rom_embed.bad.rom";
    rom[0x1234] ^= 0x01u;
    f = fopen(bad, "wb");
    CHECK(f != NULL, "cannot write %s", bad);
    if (f) {
        fwrite(rom, 1, ACE_ROM_SIZE, f);
        fclose(f);
        CHECK(embed(bad, out, &wrote) != 0, "a corrupted ROM was embedded");
        CHECK(!wrote, "a refused ROM still wrote %s", out);
    }

    /* And so is a short one. */
    f = fopen(bad, "wb");
    if (f) {
        fwrite(rom, 1, ACE_ROM_SIZE - 1, f);
        fclose(f);
        CHECK(embed(bad, out, &wrote) != 0, "a truncated ROM was embedded");
    }
    remove(bad);
    remove(out);

    TEST_DONE();
}
