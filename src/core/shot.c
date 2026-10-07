/* shot.c — a screenshot as a BMP file (shot.h). */

#include "shot.h"

#include <ctype.h>
#include <string.h>

static void le16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
    le16(p, v);
    le16(p + 2, v >> 16);
}

void shot_bmp_header(uint8_t out[SHOT_BMP_HEADER], unsigned w, unsigned h) {
    uint32_t image = SHOT_BMP_ROW(w) * h;
    memset(out, 0, SHOT_BMP_HEADER);
    out[0] = 'B';
    out[1] = 'M';
    le32(out + 2, SHOT_BMP_HEADER + image);   /* the file's size        */
    le32(out + 10, SHOT_BMP_HEADER);          /* where the pixels start */
    le32(out + 14, 40);                       /* BITMAPINFOHEADER       */
    le32(out + 18, w);
    le32(out + 22, h);                        /* positive: bottom-up    */
    le16(out + 26, 1);                        /* planes                 */
    le16(out + 28, 24);                       /* bits a pixel           */
    le32(out + 34, image);                    /* BI_RGB, uncompressed   */
    le32(out + 38, 2835);                     /* 72 dpi, as pixels/m    */
    le32(out + 42, 2835);
}

unsigned shot_bmp_row(const uint16_t *px, unsigned w, uint8_t *out) {
    unsigned n = SHOT_BMP_ROW(w);
    uint8_t *p = out;
    for (unsigned x = 0; x < w; x++) {
        unsigned r = px[x] >> 11, g = (px[x] >> 5) & 0x3Fu, b = px[x] & 0x1Fu;
        *p++ = (uint8_t)(b << 3 | b >> 2);
        *p++ = (uint8_t)(g << 2 | g >> 4);
        *p++ = (uint8_t)(r << 3 | r >> 2);
    }
    while (p < out + n) *p++ = 0;
    return n;
}

unsigned shot_index(const char *name) {
    if (strlen(name) != 12) return 0;
    static const char pre[] = "SHOT", ext[] = ".BMP";
    for (unsigned i = 0; i < 4; i++) {
        if (toupper((unsigned char)name[i]) != pre[i]) return 0;
        if (toupper((unsigned char)name[8 + i]) != ext[i]) return 0;
    }
    unsigned n = 0;
    for (unsigned i = 4; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) return 0;
        n = n * 10u + (unsigned)(name[i] - '0');
    }
    return n;
}
