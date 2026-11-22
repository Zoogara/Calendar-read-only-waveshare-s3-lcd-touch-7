#include "ha_radar_png.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct {
    int w, h, bpp;               /* bpp: bytes per pixel in the file, 3 or 4 */
} header_t;

static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

/* Walks the chunks. If idat != NULL, collects the IDAT payloads into one contiguous zlib stream: a pointer into the
 * file when there is a single IDAT, else a malloc'd copy (*owned set, the caller frees it). */
static int parse(const uint8_t *png, size_t len, header_t *hd, const uint8_t **idat, size_t *idat_len, uint8_t **owned)
{
    if (png == NULL || len < 8 + 12 || memcmp(png, SIG, 8) != 0) {
        return RADAR_PNG_BAD;
    }
    size_t pos = 8;
    int have_hdr = 0, n_idat = 0;
    size_t total_idat = 0;
    while (pos + 12 <= len) {
        uint32_t clen = be32(png + pos);
        const uint8_t *type = png + pos + 4;
        if (clen > len - pos - 12) {
            return RADAR_PNG_BAD;                       /* the chunk runs past the end of the file */
        }
        const uint8_t *data = png + pos + 8;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (clen != 13 || have_hdr) return RADAR_PNG_BAD;
            uint32_t w = be32(data), h = be32(data + 4);
            if (w == 0 || h == 0 || w > RADAR_PNG_MAX_DIM || h > RADAR_PNG_MAX_DIM) return RADAR_PNG_BAD;
            int depth = data[8], ct = data[9], comp = data[10], filt = data[11], interlace = data[12];
            if (comp != 0 || filt != 0) return RADAR_PNG_BAD;
            if (depth != 8 || (ct != 6 && ct != 2) || interlace != 0) return RADAR_PNG_UNSUPPORTED;
            hd->w = (int)w;
            hd->h = (int)h;
            hd->bpp = ct == 6 ? 4 : 3;
            have_hdr = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!have_hdr) return RADAR_PNG_BAD;
            n_idat++;
            total_idat += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + (size_t)clen;
    }
    if (!have_hdr) {
        return RADAR_PNG_BAD;
    }
    if (idat == NULL) {
        return RADAR_PNG_OK;
    }
    if (n_idat == 0) {
        return RADAR_PNG_BAD;
    }
    if (n_idat == 1) {
        pos = 8;
        while (pos + 12 <= len) {                       /* find it again: no copy needed */
            uint32_t clen = be32(png + pos);
            if (memcmp(png + pos + 4, "IDAT", 4) == 0) {
                *idat = png + pos + 8;
                *idat_len = clen;
                return RADAR_PNG_OK;
            }
            pos += 12 + (size_t)clen;
        }
        return RADAR_PNG_BAD;
    }
    uint8_t *buf = malloc(total_idat);
    if (buf == NULL) {
        return RADAR_PNG_NOMEM;
    }
    size_t o = 0;
    pos = 8;
    while (pos + 12 <= len && o < total_idat) {
        uint32_t clen = be32(png + pos);
        if (memcmp(png + pos + 4, "IDAT", 4) == 0) {
            memcpy(buf + o, png + pos + 8, clen);
            o += clen;
        }
        pos += 12 + (size_t)clen;
    }
    *owned = buf;
    *idat = buf;
    *idat_len = o;
    return RADAR_PNG_OK;
}

int radar_png_info(const uint8_t *png, size_t len, int *w, int *h)
{
    header_t hd;
    int rc = parse(png, len, &hd, NULL, NULL, NULL);
    if (rc == RADAR_PNG_OK) {
        *w = hd.w;
        *h = hd.h;
    }
    return rc;
}

/* ---- the unfiltered rows, assembled from whatever sizes the inflater flushes ---- */
typedef struct {
    header_t hd;
    size_t stride;               /* 1 filter byte + w*bpp */
    uint8_t *cur;                /* stride bytes: the row being assembled */
    uint8_t *prev;               /* w*bpp bytes: the previous row, unfiltered (zeros before the first) */
    uint8_t *rgba;               /* w*4: what the row callback gets (aliases nothing) */
    size_t fill;
    int y;
    radar_row_fn row;
    void *user;
    int result;
} dec_t;

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static int unfilter(uint8_t *line, const uint8_t *prev, size_t n, int bpp, int ftype)
{
    switch (ftype) {
    case 0:
        break;
    case 1:
        for (size_t i = (size_t)bpp; i < n; i++) line[i] = (uint8_t)(line[i] + line[i - bpp]);
        break;
    case 2:
        for (size_t i = 0; i < n; i++) line[i] = (uint8_t)(line[i] + prev[i]);
        break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            int left = i >= (size_t)bpp ? line[i - bpp] : 0;
            line[i] = (uint8_t)(line[i] + ((left + prev[i]) >> 1));
        }
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            int left = i >= (size_t)bpp ? line[i - bpp] : 0, up = prev[i], ul = i >= (size_t)bpp ? prev[i - bpp] : 0;
            line[i] = (uint8_t)(line[i] + paeth(left, up, ul));
        }
        break;
    default:
        return 0;
    }
    return 1;
}

static int put_cb(const void *buf, int len, void *user)
{
    dec_t *d = (dec_t *)user;
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = len > 0 ? (size_t)len : 0;
    while (n > 0) {
        if (d->y >= d->hd.h) {
            return 1;                                   /* more output than the image needs: ignore the excess */
        }
        size_t take = d->stride - d->fill;
        if (take > n) take = n;
        memcpy(d->cur + d->fill, p, take);
        d->fill += take;
        p += take;
        n -= take;
        if (d->fill == d->stride) {
            size_t rowb = d->stride - 1;
            if (!unfilter(d->cur + 1, d->prev, rowb, d->hd.bpp, d->cur[0])) {
                d->result = RADAR_PNG_BAD;
                return 0;
            }
            memcpy(d->prev, d->cur + 1, rowb);
            const uint8_t *src = d->cur + 1;
            if (d->hd.bpp == 4) {
                src = d->cur + 1;
            } else {
                for (int x = 0; x < d->hd.w; x++) {
                    d->rgba[4 * x + 0] = src[3 * x + 0];
                    d->rgba[4 * x + 1] = src[3 * x + 1];
                    d->rgba[4 * x + 2] = src[3 * x + 2];
                    d->rgba[4 * x + 3] = 255;
                }
                src = d->rgba;
            }
            int keep_going = d->row(d->user, d->y, src);
            d->y++;
            d->fill = 0;
            if (!keep_going) {
                d->result = RADAR_PNG_STOPPED;
                return 0;
            }
        }
    }
    return 1;
}

int radar_png_rows(const uint8_t *png, size_t len, radar_row_fn row, void *user)
{
    dec_t d;
    memset(&d, 0, sizeof(d));
    const uint8_t *idat = NULL;
    size_t idat_len = 0;
    uint8_t *owned = NULL;
    int rc = parse(png, len, &d.hd, &idat, &idat_len, &owned);
    if (rc != RADAR_PNG_OK) {
        return rc;
    }
    size_t rowb = (size_t)d.hd.w * (size_t)d.hd.bpp;
    d.stride = rowb + 1;
    d.cur = malloc(d.stride);
    d.prev = calloc(1, rowb);
    d.rgba = malloc((size_t)d.hd.w * 4);
    d.row = row;
    d.user = user;
    d.result = RADAR_PNG_OK;
    if (d.cur == NULL || d.prev == NULL || d.rgba == NULL) {
        rc = RADAR_PNG_NOMEM;
    } else {
        int ok = radar_zlib_inflate(idat, idat_len, put_cb, &d);
        if (d.result != RADAR_PNG_OK) {
            rc = d.result;                              /* a bad filter byte, or the callback stopping */
        } else if (!ok || d.y != d.hd.h) {
            rc = RADAR_PNG_BAD;                         /* the stream failed, or ended short of the image */
        } else {
            rc = RADAR_PNG_OK;
        }
    }
    free(d.cur);
    free(d.prev);
    free(d.rgba);
    free(owned);
    return rc;
}
