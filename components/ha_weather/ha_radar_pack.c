#include "ha_radar_pack.h"
#include <stdlib.h>
#include <string.h>

/* Window pixel X (0-based, in the scaled frame: window x plus the centring offset) maps to the continuous
 * source position  sx = ((X + 0.5) * DEN / NUM) - 0.5 = (2*X*DEN + DEN - NUM) / (2*NUM)  (pixel centres
 * aligned). With 4/3 that is n/8 with n = 6X - 1. These work for any NUM/DEN so the constants can change. */
static void tap(int X, int src_max, int *i0, int *i1, int *frac256)
{
    long num = 2L * X * RADAR_SCALE_DEN + RADAR_SCALE_DEN - RADAR_SCALE_NUM;     /* sx = num / den */
    long den = 2L * RADAR_SCALE_NUM;
    long fl = num >= 0 ? num / den : -((-num + den - 1) / den);                  /* floor */
    long rem = num - fl * den;                                                   /* 0 .. den-1 */
    int f = (int)((rem * 256) / den);
    long a = fl, b = fl + 1;
    if (a < 0) a = 0;
    if (a > src_max) a = src_max;
    if (b < 0) b = 0;
    if (b > src_max) b = src_max;
    *i0 = (int)a;
    *i1 = (int)b;
    *frac256 = f;
}

static int win_off_x(void)
{
    return (RADAR_SRC_SIZE * RADAR_SCALE_NUM / RADAR_SCALE_DEN - RADAR_WIN_W) / 2;
}

static int win_off_y(void)
{
    return (RADAR_SRC_SIZE * RADAR_SCALE_NUM / RADAR_SCALE_DEN - RADAR_WIN_H) / 2;
}

radar_crop_t radar_crop(void)
{
    int ox = win_off_x(), oy = win_off_y();
    int a, b, f;
    radar_crop_t c;
    tap(ox, RADAR_SRC_SIZE - 1, &a, &b, &f);
    c.x0 = a;
    tap(ox + RADAR_WIN_W - 1, RADAR_SRC_SIZE - 1, &a, &b, &f);
    c.w = b - c.x0 + 1;
    tap(oy, RADAR_SRC_SIZE - 1, &a, &b, &f);
    c.y0 = a;
    tap(oy + RADAR_WIN_H - 1, RADAR_SRC_SIZE - 1, &a, &b, &f);
    c.h = b - c.y0 + 1;
    return c;
}

size_t radar_pack_bytes(const radar_crop_t *crop)
{
    return sizeof(radar_pack_t) + (size_t)crop->w * (size_t)crop->h;
}

static uint32_t rgba_to_argb(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* Fully transparent pixels all mean the same thing whatever their (meaningless) colour. */
static uint32_t canon(uint32_t argb)
{
    return (argb >> 24) == 0 ? 0u : argb;
}

static long dist2(uint32_t a, uint32_t b)
{
    long d = 0;
    for (int s = 0; s < 32; s += 8) {
        long x = (long)((a >> s) & 0xFF) - (long)((b >> s) & 0xFF);
        d += x * x;
    }
    return d;
}

static unsigned hash_of(uint32_t argb)
{
    return (argb * 2654435761u) >> 23;      /* 9 bits: 512 slots for at most 256 colours */
}

int radar_pack_begin(radar_pack_stream_t *ps, radar_pack_t *out, int src_w, int src_h, const radar_crop_t *crop)
{
    if (ps == NULL || out == NULL || crop == NULL || crop->w <= 0 || crop->h <= 0 || crop->x0 < 0 || crop->y0 < 0 ||
        crop->x0 + crop->w > src_w || crop->y0 + crop->h > src_h || src_w > 65535 || src_h > 65535) {
        return -1;
    }
    memset(ps, 0, sizeof(*ps));
    ps->out = out;
    ps->crop = *crop;
    ps->src_w = src_w;
    ps->src_h = src_h;
    ps->last_argb = 0xFFFFFFFFu;
    memset(out, 0, sizeof(*out));
    out->src_w = (uint16_t)src_w;
    out->src_h = (uint16_t)src_h;
    out->w = (uint16_t)crop->w;
    out->h = (uint16_t)crop->h;
    return 0;
}

void radar_pack_row(radar_pack_stream_t *ps, int y, const uint8_t *rgba_row)
{
    const radar_crop_t *c = &ps->crop;
    if (y < c->y0 || y >= c->y0 + c->h) {
        return;
    }
    radar_pack_t *out = ps->out;
    uint8_t *dst = out->idx + (size_t)(y - c->y0) * c->w;
    const uint8_t *px = rgba_row + 4 * (size_t)c->x0;
    for (int x = 0; x < c->w; x++, px += 4) {
        uint32_t col = canon(rgba_to_argb(px));
        if (col == ps->last_argb) {
            dst[x] = (uint8_t)ps->last_idx;
            continue;
        }
        unsigned h = hash_of(col);
        int found = -1;
        while (ps->hash[h]) {
            if (out->pal[ps->hash[h] - 1] == col) {
                found = ps->hash[h] - 1;
                break;
            }
            h = (h + 1) & 511;
        }
        if (found < 0) {
            if (out->npal < RADAR_PAL_MAX) {
                found = out->npal++;
                out->pal[found] = col;
                ps->hash[h] = (uint16_t)(found + 1);
            } else {                              /* palette full: the nearest colour kept */
                long best = -1;
                for (int i = 0; i < out->npal; i++) {
                    long d = dist2(col, out->pal[i]);
                    if (best < 0 || d < best) {
                        best = d;
                        found = i;
                    }
                }
                ps->merged++;
            }
        }
        dst[x] = (uint8_t)found;
        ps->last_argb = col;
        ps->last_idx = found;
    }
}

int radar_pack_end(radar_pack_stream_t *ps)
{
    return ps->merged;
}

int radar_pack(const uint8_t *rgba, int src_w, int src_h, const radar_crop_t *crop, radar_pack_t *out)
{
    if (rgba == NULL) {
        return -1;
    }
    radar_pack_stream_t *ps = malloc(sizeof(*ps));
    if (ps == NULL) {
        return -1;
    }
    int rc = radar_pack_begin(ps, out, src_w, src_h, crop);
    if (rc == 0) {
        for (int y = crop->y0; y < crop->y0 + crop->h; y++) {
            radar_pack_row(ps, y, rgba + (size_t)y * src_w * 4);
        }
        rc = radar_pack_end(ps);
    }
    free(ps);
    return rc;
}

static uint16_t to565(int r, int g, int b)
{
    int r5 = (r * 31 + 127) / 255, g6 = (g * 63 + 127) / 255, b5 = (b * 31 + 127) / 255;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

void radar_expand(const radar_pack_t *p, radar_scratch_t *sc, uint16_t *rgb565, uint8_t *alpha, bool swap_bytes)
{
    const int ox = win_off_x(), oy = win_off_y();
    const radar_crop_t c = radar_crop();
    const int smax_x = p->src_w - 1, smax_y = p->src_h - 1;

    /* The taps and fractions depend only on the column, so work them out once. */
    int *cx0 = sc->x0, *cx1 = sc->x1, *cfx = sc->fx;
    for (int x = 0; x < RADAR_WIN_W; x++) {
        int a, b, f;
        tap(ox + x, smax_x, &a, &b, &f);
        cx0[x] = a - c.x0;
        cx1[x] = b - c.x0;
        cfx[x] = f;
    }
    /* Palette as premultiplied components (so a blended pixel is a few table reads and multiplies), and as
     * final output values (so a pixel whose four neighbours are the same colour - most of them: the
     * transparent background and the inside of a rain cell - is two table reads). */
    uint32_t *pa = sc->pa, *pr = sc->pr, *pg = sc->pg, *pb = sc->pb;
    for (int i = 0; i < RADAR_PAL_MAX; i++) {
        uint32_t argb = i < p->npal ? p->pal[i] : 0;
        uint32_t a = argb >> 24;
        pa[i] = a;
        pr[i] = ((argb >> 16) & 0xFF) * a;      /* 0..65025 */
        pg[i] = ((argb >> 8) & 0xFF) * a;
        pb[i] = (argb & 0xFF) * a;
        uint16_t v = a ? to565((int)((argb >> 16) & 0xFF), (int)((argb >> 8) & 0xFF), (int)(argb & 0xFF)) : 0;
        sc->c565[i] = swap_bytes ? (uint16_t)((v >> 8) | (v << 8)) : v;
        sc->a8[i] = (uint8_t)a;
    }

    for (int y = 0; y < RADAR_WIN_H; y++) {
        int ya, yb, fy;
        tap(oy + y, smax_y, &ya, &yb, &fy);
        const uint8_t *r0 = p->idx + (size_t)(ya - c.y0) * p->w;
        const uint8_t *r1 = p->idx + (size_t)(yb - c.y0) * p->w;
        for (int x = 0; x < RADAR_WIN_W; x++) {
            unsigned i00 = r0[cx0[x]], i10 = r0[cx1[x]], i01 = r1[cx0[x]], i11 = r1[cx1[x]];
            size_t o = (size_t)y * RADAR_WIN_W + x;
            if (i00 == i10 && i00 == i01 && i00 == i11) {      /* all four the same: no blending to do */
                rgb565[o] = sc->c565[i00];
                alpha[o] = sc->a8[i00];
                continue;
            }
            int fx = cfx[x];
            /* weights in 1/65536; every sum below is at most 65536 * 65025, which fits 32 bits */
            uint32_t w00 = (uint32_t)(256 - fx) * (256 - fy), w10 = (uint32_t)fx * (256 - fy);
            uint32_t w01 = (uint32_t)(256 - fx) * fy, w11 = (uint32_t)fx * fy;
            uint32_t A = w00 * pa[i00] + w10 * pa[i10] + w01 * pa[i01] + w11 * pa[i11];
            if (A == 0) {
                rgb565[o] = 0;
                alpha[o] = 0;
                continue;
            }
            uint32_t R = w00 * pr[i00] + w10 * pr[i10] + w01 * pr[i01] + w11 * pr[i11];
            uint32_t G = w00 * pg[i00] + w10 * pg[i10] + w01 * pg[i01] + w11 * pg[i11];
            uint32_t B = w00 * pb[i00] + w10 * pb[i10] + w01 * pb[i01] + w11 * pb[i11];
            /* un-premultiply: colour = sum(w*a*c) / sum(w*a); alpha = sum(w*a) / 65536 */
            int r = (int)((R + A / 2) / A), g = (int)((G + A / 2) / A), b = (int)((B + A / 2) / A);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            uint16_t v = to565(r, g, b);
            rgb565[o] = swap_bytes ? (uint16_t)((v >> 8) | (v << 8)) : v;
            alpha[o] = (uint8_t)((A + 32768) >> 16);
        }
    }
}
