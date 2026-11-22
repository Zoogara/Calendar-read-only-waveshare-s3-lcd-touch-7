#pragma once
/*
 * A streaming PNG decoder for the radar page (pure C, no ESP-IDF - host-tested): it hands over one unfiltered RGBA row at
 * a time, so the whole image never exists. lodepng - which LVGL ships - decodes to a full image and holds the inflated scanlines
 * beside it, about 1.2 MB of PSRAM for a 384x384 frame, and when PSRAM was fragmented a decode failed with "memory
 * allocation failed" (error 83). This needs a 32 KB inflate window and two rows.
 *
 * Supported: 8-bit RGBA or RGB, not interlaced (what RainViewer's tiles and NASA GIBS's WMS images are). Anything else is
 * refused with RADAR_PNG_UNSUPPORTED, so a change at the source shows up as a clear error and not garbage.
 *
 * The zlib inflater is provided by the platform: the display uses the ROM's miniz (ha_radar_inflate_rom.c), the host tests
 * zlib. It must call put() with the output in order, in pieces of any size, and stop if put() returns 0.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*radar_put_fn)(const void *buf, int len, void *user);

/* Inflate the zlib stream in[0..in_len), flushing the output through put(). Returns 1 on success, 0 on failure
 * (including put() returning 0). */
int radar_zlib_inflate(const uint8_t *in, size_t in_len, radar_put_fn put, void *user);

enum {
    RADAR_PNG_OK = 0,
    RADAR_PNG_BAD,            /* not a well-formed PNG, truncated, bad filter byte, or the pixel data is the wrong length */
    RADAR_PNG_UNSUPPORTED,    /* a PNG, but not 8-bit RGB/RGBA non-interlaced */
    RADAR_PNG_NOMEM,
    RADAR_PNG_STOPPED,        /* the row callback asked to stop */
};

#define RADAR_PNG_MAX_DIM 2048

/* Reads the header only: width and height. RADAR_PNG_OK or an error. */
int radar_png_info(const uint8_t *png, size_t len, int *w, int *h);

/* Called with each row in order: y = 0..h-1 and w*4 bytes (R,G,B,A; alpha 255 for an RGB image). Return 0 to stop. */
typedef int (*radar_row_fn)(void *user, int y, const uint8_t *rgba_row);

/* Decodes the whole image row by row. RADAR_PNG_OK once every row has been delivered. */
int radar_png_rows(const uint8_t *png, size_t len, radar_row_fn row, void *user);

#ifdef __cplusplus
}
#endif
