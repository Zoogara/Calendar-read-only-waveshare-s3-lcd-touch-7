/* ha_radar_pack.c: packing RainViewer frames into indexed form and expanding them for the 380x330 window.
 * The real frames come from data/ (see make_radar_fixtures.py); the golden images are what Pillow makes of
 * the same frames, an independent implementation to compare the expansion against. Tests run from
 * tests/host, so the data/ path is relative to that. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ha_radar_pack.h"
#include "test.h"

static radar_scratch_t sc;       /* the tests share one, as the firmware does */

#define SRC_BYTES ((size_t)RADAR_SRC_SIZE * RADAR_SRC_SIZE * 4)
#define WIN_PX    ((size_t)RADAR_WIN_W * RADAR_WIN_H)

/* Reads a zlib-compressed file and inflates it to exactly want bytes; NULL if it can't. */
static uint8_t *load_z(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *z = malloc((size_t)n), *out = malloc(want);
    size_t rd = fread(z, 1, (size_t)n, f);
    fclose(f);
    uLongf got = want;
    if (rd != (size_t)n || uncompress(out, &got, z, (uLong)n) != Z_OK || got != want) {
        free(out);
        out = NULL;
    }
    free(z);
    return out;
}

static void crop_geometry(void)
{
    radar_crop_t c = radar_crop();
    /* window offset in the 512 px scaled frame is (66, 91); see the header for how the taps work out */
    CHECK_INT(c.x0, 49);
    CHECK_INT(c.y0, 68);
    CHECK_INT(c.w, 286);
    CHECK_INT(c.h, 248);
    CHECK(c.x0 + c.w <= RADAR_SRC_SIZE && c.y0 + c.h <= RADAR_SRC_SIZE);
    CHECK_INT(radar_pack_bytes(&c), sizeof(radar_pack_t) + 286 * 248);
}

static void pack_is_lossless_on_real_frames(void)
{
    radar_crop_t c = radar_crop();
    for (int n = 1; n <= 2; n++) {
        char path[64];
        snprintf(path, sizeof(path), "data/radar%d.rgba.z", n);
        uint8_t *rgba = load_z(path, SRC_BYTES);
        CHECK(rgba != NULL);
        if (rgba == NULL) continue;
        radar_pack_t *p = malloc(radar_pack_bytes(&c));
        int merged = radar_pack(rgba, RADAR_SRC_SIZE, RADAR_SRC_SIZE, &c, p);
        CHECK_INT(merged, 0);
        CHECK(p->npal > 1 && p->npal < 100);       /* the rain palette: dozens of colours, not thousands */
        CHECK_INT(p->w, c.w);
        CHECK_INT(p->h, c.h);
        /* every cropped pixel comes back exactly (transparent pixels are all "transparent", whatever RGB) */
        int bad = 0;
        for (int y = 0; y < c.h; y++) {
            for (int x = 0; x < c.w; x++) {
                const uint8_t *s = rgba + ((size_t)(c.y0 + y) * RADAR_SRC_SIZE + c.x0 + x) * 4;
                uint32_t want = ((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
                if (s[3] == 0) want = 0;
                uint8_t i = p->idx[(size_t)y * c.w + x];
                if (i >= p->npal || p->pal[i] != want) bad++;
            }
        }
        CHECK_INT(bad, 0);
        free(p);
        free(rgba);
    }
}

static void pack_merges_when_there_are_too_many_colours(void)
{
    /* 40x40 crop of a 64x64 image with 1600 distinct opaque colours */
    enum { N = 64 };
    uint8_t *img = calloc(N * N, 4);
    for (int i = 0; i < N * N; i++) {
        img[4 * i + 0] = (uint8_t)(i & 0xFF);
        img[4 * i + 1] = (uint8_t)((i >> 3) & 0xFF);
        img[4 * i + 2] = (uint8_t)((i * 7) & 0xFF);
        img[4 * i + 3] = 255;
    }
    radar_crop_t c = {10, 10, 40, 40};
    radar_pack_t *p = malloc(radar_pack_bytes(&c));
    int merged = radar_pack(img, N, N, &c, p);
    CHECK(merged > 0);
    CHECK_INT(p->npal, RADAR_PAL_MAX);
    int bad = 0;
    for (int i = 0; i < c.w * c.h; i++) {
        if (p->idx[i] >= p->npal) bad++;
    }
    CHECK_INT(bad, 0);
    free(p);
    free(img);
}

static void pack_rejects_bad_arguments(void)
{
    uint8_t px[4 * 4 * 4] = {0};
    radar_crop_t ok = {0, 0, 4, 4}, outside = {2, 2, 4, 4}, empty = {0, 0, 0, 4};
    radar_pack_t *p = malloc(radar_pack_bytes(&ok));
    CHECK_INT(radar_pack(px, 4, 4, &ok, p), 0);
    CHECK_INT(radar_pack(px, 4, 4, &outside, p), -1);
    CHECK_INT(radar_pack(px, 4, 4, &empty, p), -1);
    CHECK_INT(radar_pack(NULL, 4, 4, &ok, p), -1);
    CHECK_INT(radar_pack(px, 4, 4, NULL, p), -1);
    CHECK_INT(radar_pack(px, 4, 4, &ok, NULL), -1);
    free(p);
}

/* A full-size synthetic frame, packed and expanded. */
static void expand_synthetic(const uint8_t *rgba, uint16_t *c565, uint8_t *a)
{
    radar_crop_t c = radar_crop();
    radar_pack_t *p = malloc(radar_pack_bytes(&c));
    CHECK_INT(radar_pack(rgba, RADAR_SRC_SIZE, RADAR_SRC_SIZE, &c, p), 0);
    radar_expand(p, &sc, c565, a, false);
    free(p);
}

static void expand_flat_colours_stay_flat(void)
{
    uint8_t *img = malloc(SRC_BYTES);
    uint16_t *c565 = malloc(WIN_PX * 2);
    uint8_t *a = malloc(WIN_PX);
    for (size_t i = 0; i < (size_t)RADAR_SRC_SIZE * RADAR_SRC_SIZE; i++) {      /* opaque pure red */
        img[4 * i] = 255; img[4 * i + 1] = 0; img[4 * i + 2] = 0; img[4 * i + 3] = 255;
    }
    expand_synthetic(img, c565, a);
    int bad = 0;
    for (size_t i = 0; i < WIN_PX; i++) {
        if (c565[i] != 0xF800 || a[i] != 255) bad++;
    }
    CHECK_INT(bad, 0);

    memset(img, 0, SRC_BYTES);                                                   /* fully transparent */
    expand_synthetic(img, c565, a);
    bad = 0;
    for (size_t i = 0; i < WIN_PX; i++) {
        if (a[i] != 0) bad++;
    }
    CHECK_INT(bad, 0);
    free(img); free(c565); free(a);
}

static void expand_edges_keep_their_colour(void)
{
    /* the left half opaque red, the right half transparent (and black): the edge must fade out in alpha
     * but never darken - transparent black must not bleed into the colour */
    uint8_t *img = calloc(1, SRC_BYTES);
    uint16_t *c565 = malloc(WIN_PX * 2);
    uint8_t *a = malloc(WIN_PX);
    for (int y = 0; y < RADAR_SRC_SIZE; y++) {
        for (int x = 0; x < RADAR_SRC_SIZE / 2; x++) {
            uint8_t *p = img + ((size_t)y * RADAR_SRC_SIZE + x) * 4;
            p[0] = 255; p[3] = 255;
        }
    }
    expand_synthetic(img, c565, a);
    int dark = 0, nonmono = 0, partial = 0;
    for (int y = 0; y < RADAR_WIN_H; y++) {
        int prev = 256;
        for (int x = 0; x < RADAR_WIN_W; x++) {
            size_t i = (size_t)y * RADAR_WIN_W + x;
            if (a[i] > 0 && c565[i] != 0xF800) dark++;
            if (a[i] > prev) nonmono++;
            if (a[i] > 0 && a[i] < 255) partial++;
            prev = a[i];
        }
    }
    CHECK_INT(dark, 0);
    CHECK_INT(nonmono, 0);
    CHECK(partial > 0 && partial < 4 * RADAR_WIN_H);      /* a thin soft edge, one or two pixels wide */
    free(img); free(c565); free(a);
}

static void expand_swap_is_byte_swapped(void)
{
    uint8_t *rgba = load_z("data/radar2.rgba.z", SRC_BYTES);
    CHECK(rgba != NULL);
    if (rgba == NULL) return;
    radar_crop_t c = radar_crop();
    radar_pack_t *p = malloc(radar_pack_bytes(&c));
    radar_pack(rgba, RADAR_SRC_SIZE, RADAR_SRC_SIZE, &c, p);
    uint16_t *n = malloc(WIN_PX * 2), *s = malloc(WIN_PX * 2);
    uint8_t *an = malloc(WIN_PX), *as = malloc(WIN_PX);
    radar_expand(p, &sc, n, an, false);
    radar_expand(p, &sc, s, as, true);
    int bad = 0;
    for (size_t i = 0; i < WIN_PX; i++) {
        if (s[i] != (uint16_t)((n[i] >> 8) | (n[i] << 8)) || an[i] != as[i]) bad++;
    }
    CHECK_INT(bad, 0);
    free(n); free(s); free(an); free(as); free(p); free(rgba);
}

static void expand_matches_pillow_on_real_frames(void)
{
    radar_crop_t c = radar_crop();
    for (int n = 1; n <= 2; n++) {
        char path[64];
        snprintf(path, sizeof(path), "data/radar%d.rgba.z", n);
        uint8_t *rgba = load_z(path, SRC_BYTES);
        snprintf(path, sizeof(path), "data/golden%d.rgba.z", n);
        uint8_t *gold = load_z(path, WIN_PX * 4);
        CHECK(rgba != NULL && gold != NULL);
        if (rgba == NULL || gold == NULL) { free(rgba); free(gold); continue; }
        radar_pack_t *p = malloc(radar_pack_bytes(&c));
        radar_pack(rgba, RADAR_SRC_SIZE, RADAR_SRC_SIZE, &c, p);
        uint16_t *c565 = malloc(WIN_PX * 2);
        uint8_t *a = malloc(WIN_PX);
        radar_expand(p, &sc, c565, a, false);

        long long alpha_err = 0, col_err = 0, col_n = 0;
        int alpha_max = 0, col_max = 0, faint_max = 0, rain_px = 0;
        for (size_t i = 0; i < WIN_PX; i++) {
            const uint8_t *g = gold + 4 * i;
            int d = abs((int)a[i] - (int)g[3]);
            alpha_err += d;
            if (d > alpha_max) alpha_max = d;
            if (g[3] > 0) rain_px++;
            if (g[3] > 24 && a[i] > 24) {          /* colour only means something where there is some rain */
                uint16_t v = c565[i];
                int r = ((v >> 11) & 31) * 255 / 31, gg = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
                int e = abs(r - g[0]) + abs(gg - g[1]) + abs(b - g[2]);
                col_err += e;
                col_n++;
                if (g[3] >= 100 && a[i] >= 100 && e > col_max) col_max = e;      /* where it is clearly visible */
                if (e > faint_max) faint_max = e;
            }
        }
        double alpha_mean = (double)alpha_err / WIN_PX;
        double col_mean = col_n ? (double)col_err / (double)col_n / 3.0 : 0;
        if (getenv("RADAR_STATS")) {
            printf("  frame %d: rain pixels %d, alpha mean err %.3f max %d, colour mean err %.3f (per channel), max %d (summed, alpha >= 100) / %d (any)\n",
                   n, rain_px, alpha_mean, alpha_max, col_mean, col_max, faint_max);
        }
        CHECK(rain_px > 1000);
        CHECK(alpha_mean < 0.5);
        CHECK(alpha_max <= 3);
        CHECK(col_mean < 2.0);             /* RGB565 itself costs up to ~4 per channel */
        CHECK(col_max <= 16);               /* RGB565 alone is at most 4+2+4 summed; the rest is rounding */
        CHECK(faint_max <= 40);             /* very faint pixels: dividing by a small alpha magnifies rounding */
        free(c565); free(a); free(p); free(rgba); free(gold);
    }
}

void test_radar(void)
{
    RUN(crop_geometry);
    RUN(pack_is_lossless_on_real_frames);
    RUN(pack_merges_when_there_are_too_many_colours);
    RUN(pack_rejects_bad_arguments);
    RUN(expand_flat_colours_stay_flat);
    RUN(expand_edges_keep_their_colour);
    RUN(expand_swap_is_byte_swapped);
    RUN(expand_matches_pillow_on_real_frames);
}
