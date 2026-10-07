/* test_shot.c — screenshots as BMP files (shot.h, design.md §12).
 *
 * A screen rendered by the firmware's row generator, framed as the panel
 * frames it, is encoded row by row as the port encodes it, then read
 * back by a reader that knows only the BMP format, and every pixel
 * compared with the generator's. Needs no ROM run: the character set is
 * the ROM's, expanded as the menu's is.
 *
 *   test_shot            check
 *   test_shot --write D  also write the image to D/SHOT0001.bmp, to look at
 */

#include <stdbool.h>
#include <string.h>

#include "ace_rom.h"
#include "font.h"
#include "render.h"
#include "shot.h"
#include "test_util.h"

/* The panel's size and the guest's place on it, as config.h has them;
 * a grey bar where the status line goes. */
#define W ACE_PANEL_W
#define H ACE_PANEL_H
#define GREY 0x9492u   /* display.c's TEXT_INK, (0x90, 0x90, 0x90) */

static uint8_t  s_charset[ACE_CHARSET_BYTES];
static uint8_t  s_screen[ACE_SCREEN_BYTES];
static render_t s_r;
static uint8_t  s_file[SHOT_BMP_HEADER + SHOT_BMP_ROW(W) * H];

static void panel_row(unsigned y, uint16_t *px) {
    for (unsigned x = 0; x < W; x++) px[x] = ACE_PAPER_RGB565;
    if (y >= ACE_SCREEN_Y && y < ACE_SCREEN_Y + ACE_PIXEL_H)
        render_row(&s_r, s_screen, s_charset, y - ACE_SCREEN_Y, px + ACE_SCREEN_X);
    else if (y >= ACE_STATUS_Y && y < ACE_STATUS_Y + ACE_GLYPH_ROWS)
        for (unsigned x = 8; x < 64; x++) px[x] = GREY;
}

/* As the port writes it: the header, then the rows from the foot up. */
static size_t encode(void) {
    uint8_t *p = s_file;
    shot_bmp_header(p, W, H);
    p += SHOT_BMP_HEADER;
    uint16_t px[W];
    for (unsigned y = H; y-- > 0;) {
        panel_row(y, px);
        p += shot_bmp_row(px, W, p);
    }
    return (size_t)(p - s_file);
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

int main(int argc, char **argv) {
    const char *write_dir = (argc == 3 && strcmp(argv[1], "--write") == 0) ? argv[2] : NULL;

    /* ---- names ----------------------------------------------------------- */
    CHECK(shot_index("SHOT0001.bmp") == 1, "SHOT0001.bmp");
    CHECK(shot_index("shot0042.BMP") == 42, "either case");
    CHECK(shot_index("SHOT9999.bmp") == SHOT_LAST, "the last");
    CHECK(shot_index("SHOT001.bmp") == 0, "three digits");
    CHECK(shot_index("SHOT00a1.bmp") == 0, "not a number");
    CHECK(shot_index("SHOT0001.bmp.new") == 0, "a partial one");
    CHECK(shot_index("TAPE0001.bmp") == 0, "another name");
    CHECK(shot_index("SHOT0001.ppm") == 0, "another kind");

    /* ---- a row ----------------------------------------------------------- */
    /* White and black go to full scale and zero; each channel lands in its
     * own byte, blue first; an odd width is padded with zeros to 4. */
    static const uint16_t row[3] = { 0xFFFFu, 0xF800u, 0x001Fu };
    uint8_t out[SHOT_BMP_ROW(3) + 4];
    memset(out, 0xAA, sizeof out);
    CHECK(SHOT_BMP_ROW(3) == 12 && SHOT_BMP_ROW(1) == 4 && SHOT_BMP_ROW(W) == 3 * W,
          "row sizes");
    CHECK(shot_bmp_row(row, 3, out) == 12, "three pixels take 12 bytes");
    static const uint8_t want[12] = { 0xFF, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0 };
    CHECK(memcmp(out, want, sizeof want) == 0, "white, red, blue: %02X %02X %02X / "
          "%02X %02X %02X / %02X %02X %02X", out[0], out[1], out[2], out[3], out[4],
          out[5], out[6], out[7], out[8]);
    CHECK(out[12] == 0xAA, "wrote past the row");

    /* ---- the panel, through and back ------------------------------------- */
    render_init(&s_r, ACE_INK_RGB565, ACE_PAPER_RGB565);
    font_from_rom(ace_rom, s_charset);
    memset(s_screen, ' ', sizeof s_screen);
    static const char *const text = "Screenshot: F6";
    memcpy(s_screen, text, strlen(text));
    for (unsigned i = 0; i < strlen(text); i++)
        s_screen[23 * 32 + 9 + i] = (uint8_t)(text[i] | 0x80u);

    size_t n = encode();
    CHECK(n == sizeof s_file, "%zu bytes, want %zu", n, sizeof s_file);
    CHECK(s_file[0] == 'B' && s_file[1] == 'M', "magic");
    CHECK(rd32(s_file + 2) == n, "file size %u", (unsigned)rd32(s_file + 2));
    uint32_t off = rd32(s_file + 10);
    CHECK(rd32(s_file + 14) == 40, "BITMAPINFOHEADER");
    CHECK(rd32(s_file + 18) == W && rd32(s_file + 22) == H, "%ux%u",
          (unsigned)rd32(s_file + 18), (unsigned)rd32(s_file + 22));
    CHECK(rd16(s_file + 26) == 1 && rd16(s_file + 28) == 24, "planes and depth");
    CHECK(rd32(s_file + 30) == 0, "uncompressed");

    /* Pixel (x, y) from the top-left, as a viewer reads it. */
    unsigned bad = 0, white = 0, grey = 0;
    uint16_t px[W];
    for (unsigned y = 0; y < H && off == SHOT_BMP_HEADER; y++) {
        panel_row(y, px);
        const uint8_t *r = s_file + off + (size_t)(H - 1u - y) * SHOT_BMP_ROW(W);
        for (unsigned x = 0; x < W; x++) {
            uint8_t b = r[3 * x], g = r[3 * x + 1], rr = r[3 * x + 2];
            uint16_t back = (uint16_t)((rr >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            if (back != px[x]) bad++;
            white += rr == 0xFF && g == 0xFF && b == 0xFF;
            grey += px[x] == GREY;
        }
    }
    CHECK(bad == 0, "%u pixels differ from the generator's", bad);
    CHECK(white > 0 && grey == 56u * 8u, "the image is not blank: %u white, %u grey", white, grey);
    /* The top-left glyph 'S' starts in the guest's corner, not the file's. */
    const uint8_t *top = s_file + off + (size_t)(H - 1u - ACE_SCREEN_Y) * SHOT_BMP_ROW(W);
    CHECK(top[0] == 0 && top[1] == 0 && top[2] == 0, "the border is black");

    if (write_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/SHOT0001.bmp", write_dir);
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(s_file, 1, n, f) == n && fclose(f) == 0, "could not write %s", path);
        printf("wrote %s\n", path);
    }

    TEST_DONE();
}
