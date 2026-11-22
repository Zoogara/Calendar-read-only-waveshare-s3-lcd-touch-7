/* ha_radar_png.c: the streaming PNG decoder, with zlib standing in for the display's ROM inflater. The reference is the
 * same real frames kept as raw RGBA (data/radarN.rgba.z), so a decode is compared byte for byte, not against itself. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ha_radar_pack.h"
#include "ha_radar_png.h"
#include "test.h"

/* The platform hook, as the display's ROM version does it: a 32 KB window, flushed in order. */
int radar_zlib_inflate(const uint8_t *in, size_t in_len, radar_put_fn put, void *user)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) return 0;
    static uint8_t win[32768];
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    int ok = 0;
    for (;;) {
        z.next_out = win;
        z.avail_out = sizeof(win);
        int rc = inflate(&z, Z_NO_FLUSH);
        size_t got = sizeof(win) - z.avail_out;
        if (got > 0 && !put(win, (int)got, user)) break;
        if (rc == Z_STREAM_END) { ok = 1; break; }
        if (rc != Z_OK && rc != Z_BUF_ERROR) break;
        if (rc == Z_BUF_ERROR && got == 0) break;     /* needs input that isn't there: truncated */
    }
    inflateEnd(&z);
    return ok;
}

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { printf("  cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint8_t *load_z(const char *path, size_t want)
{
    size_t n;
    uint8_t *z = load_file(path, &n);
    if (z == NULL) return NULL;
    uint8_t *out = malloc(want);
    uLongf got = want;
    if (uncompress(out, &got, z, (uLong)n) != Z_OK || got != want) { free(out); out = NULL; }
    free(z);
    return out;
}

typedef struct {
    const uint8_t *want;         /* raw RGBA reference, or NULL */
    int w;
    int rows, bad_rows, stop_after;
    uint8_t *got;                /* optional copy of everything delivered */
} cmp_t;

static int cmp_row(void *user, int y, const uint8_t *row)
{
    cmp_t *c = (cmp_t *)user;
    if (y != c->rows) c->bad_rows++;                         /* rows must arrive in order */
    if (c->want && memcmp(c->want + (size_t)y * c->w * 4, row, (size_t)c->w * 4) != 0) c->bad_rows++;
    if (c->got) memcpy(c->got + (size_t)y * c->w * 4, row, (size_t)c->w * 4);
    c->rows++;
    return c->stop_after == 0 || c->rows < c->stop_after;
}

static void decode_matches_reference(void)
{
    static const char *files[] = {"data/radar1.png", "data/radar2.png", "data/radar1_split.png"};
    static const char *raws[] = {"data/radar1.rgba.z", "data/radar2.rgba.z", "data/radar1.rgba.z"};
    for (int i = 0; i < 3; i++) {
        size_t n;
        uint8_t *png = load_file(files[i], &n);
        uint8_t *raw = load_z(raws[i], 384 * 384 * 4);
        CHECK(png != NULL && raw != NULL);
        if (png == NULL || raw == NULL) { free(png); free(raw); continue; }
        int w = 0, h = 0;
        CHECK_INT(radar_png_info(png, n, &w, &h), RADAR_PNG_OK);
        CHECK_INT(w, 384);
        CHECK_INT(h, 384);
        cmp_t c = {.want = raw, .w = 384};
        CHECK_INT(radar_png_rows(png, n, cmp_row, &c), RADAR_PNG_OK);
        CHECK_INT(c.rows, 384);
        CHECK_INT(c.bad_rows, 0);
        free(png);
        free(raw);
    }
}

static void rgb_image_gets_opaque_alpha(void)
{
    size_t n;
    uint8_t *png = load_file("data/rgb40x30.png", &n);
    CHECK(png != NULL);
    if (png == NULL) return;
    int w, h;
    CHECK_INT(radar_png_info(png, n, &w, &h), RADAR_PNG_OK);
    CHECK_INT(w, 40);
    CHECK_INT(h, 30);
    uint8_t *got = malloc((size_t)w * h * 4);
    cmp_t c = {.w = w, .got = got};
    CHECK_INT(radar_png_rows(png, n, cmp_row, &c), RADAR_PNG_OK);
    CHECK_INT(c.bad_rows, 0);
    int bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = got + ((size_t)y * w + x) * 4;      /* the pattern make_radar_fixtures.py draws */
            if (p[0] != ((x * 6) & 255) || p[1] != ((y * 8) & 255) || p[2] != (((x + y) * 3) & 255) || p[3] != 255) bad++;
        }
    CHECK_INT(bad, 0);
    free(got);
    free(png);
}

static void rows_can_stop_the_decode(void)
{
    size_t n;
    uint8_t *png = load_file("data/radar1.png", &n);
    CHECK(png != NULL);
    if (png == NULL) return;
    cmp_t c = {.w = 384, .stop_after = 10};
    CHECK_INT(radar_png_rows(png, n, cmp_row, &c), RADAR_PNG_STOPPED);
    CHECK_INT(c.rows, 10);
    free(png);
}

static void refuses_what_it_cannot_decode(void)
{
    size_t n;
    uint8_t *png = load_file("data/radar1.png", &n);
    CHECK(png != NULL);
    if (png == NULL) return;
    cmp_t c = {.w = 384};
    int w, h;
    uint8_t *m = malloc(n);

    /* the IHDR fields: width at 16..19, height 20..23, depth 24, colour type 25, interlace 28 */
    memcpy(m, png, n); m[24] = 16;                    /* 16-bit */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_UNSUPPORTED);
    memcpy(m, png, n); m[25] = 3;                     /* palette */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_UNSUPPORTED);
    memcpy(m, png, n); m[25] = 0;                     /* grey */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_UNSUPPORTED);
    memcpy(m, png, n); m[28] = 1;                     /* Adam7 interlaced */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_UNSUPPORTED);
    CHECK_INT(radar_png_rows(m, n, cmp_row, &c), RADAR_PNG_UNSUPPORTED);
    memcpy(m, png, n); m[26] = 1;                     /* an unknown compression method */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_BAD);
    memcpy(m, png, n); m[16] = m[17] = 0; m[18] = m[19] = 0;       /* width 0 */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_BAD);
    memcpy(m, png, n); m[16] = 0x7F;                  /* absurdly wide */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_BAD);
    memcpy(m, png, n); m[1] = 'X';                    /* not a PNG */
    CHECK_INT(radar_png_info(m, n, &w, &h), RADAR_PNG_BAD);
    CHECK_INT(radar_png_info(NULL, 0, &w, &h), RADAR_PNG_BAD);
    CHECK_INT(radar_png_info(png, 5, &w, &h), RADAR_PNG_BAD);
    free(m);
    free(png);
}

static void truncated_and_corrupt_streams_fail_cleanly(void)
{
    size_t n;
    uint8_t *png = load_file("data/radar1.png", &n);
    CHECK(png != NULL);
    if (png == NULL) return;
    uint8_t *m = malloc(n);
    /* cut anywhere: never a crash (ASan runs these), and never reported as a good image */
    int accepted = 0;
    for (size_t cut = 0; cut < n; cut += 211) {
        cmp_t c = {.w = 384};
        int rc = radar_png_rows(png, cut, cmp_row, &c);
        if (rc == RADAR_PNG_OK) accepted++;
        CHECK(c.rows <= 384);
    }
    CHECK_INT(accepted, 0);
    /* flip bytes inside the compressed data: either the stream fails or the rows differ, but no overrun */
    for (size_t pos = 60; pos < n - 20; pos += 397) {
        memcpy(m, png, n);
        m[pos] ^= 0x5A;
        cmp_t c = {.w = 384};
        (void)radar_png_rows(m, n, cmp_row, &c);
        CHECK(c.rows <= 384);
    }
    /* an IDAT that claims to be longer than the file */
    memcpy(m, png, n);
    m[33] = 0x7F;                                       /* the first chunk after IHDR starts at 33 (8 + 25) */
    cmp_t c2 = {.w = 384};
    CHECK(radar_png_rows(m, n, cmp_row, &c2) != RADAR_PNG_OK);
    free(m);
    free(png);
}

static int pack_row_fn(void *u, int y, const uint8_t *row)
{
    radar_pack_row((radar_pack_stream_t *)u, y, row);
    return 1;
}

static void pack_through_the_stream_equals_pack_from_raw(void)
{
    size_t n;
    uint8_t *png = load_file("data/radar2.png", &n);
    uint8_t *raw = load_z("data/radar2.rgba.z", 384 * 384 * 4);
    CHECK(png != NULL && raw != NULL);
    if (png == NULL || raw == NULL) { free(png); free(raw); return; }
    radar_crop_t crop = radar_crop();
    radar_pack_t *a = malloc(radar_pack_bytes(&crop)), *b = malloc(radar_pack_bytes(&crop));
    CHECK_INT(radar_pack(raw, 384, 384, &crop, a), 0);

    radar_pack_stream_t *ps = malloc(sizeof(*ps));
    CHECK_INT(radar_pack_begin(ps, b, 384, 384, &crop), 0);
    CHECK_INT(radar_png_rows(png, n, pack_row_fn, ps), RADAR_PNG_OK);
    CHECK_INT(radar_pack_end(ps), 0);
    CHECK_INT(a->npal, b->npal);
    CHECK(memcmp(a->idx, b->idx, (size_t)crop.w * crop.h) == 0);       /* same indices, same palette order: same algorithm */
    CHECK(memcmp(a->pal, b->pal, sizeof(a->pal[0]) * a->npal) == 0);
    free(ps); free(a); free(b); free(png); free(raw);
}

void test_radarpng(void)
{
    RUN(decode_matches_reference);
    RUN(rgb_image_gets_opaque_alpha);
    RUN(rows_can_stop_the_decode);
    RUN(refuses_what_it_cannot_decode);
    RUN(truncated_and_corrupt_streams_fail_cleanly);
    RUN(pack_through_the_stream_equals_pack_from_raw);
}
