#pragma once
/*
 * Radar frames, kept small and drawn without an image decoder (pure C, no ESP-IDF - host-tested).
 *
 * A RainViewer frame is a 384x384 RGBA PNG whose pixels use only a few dozen distinct colours (a fixed rain
 * palette), and the display shows a 380x330 window onto it scaled up 4/3 about its centre. So a frame is
 * kept as the cropped part of the source it needs (about 286x248) in 8-bit indexed form - lossless, about
 * 72 KB - and each animation step is expanded by radar_expand() into one shared 380x330 RGB565 + alpha
 * working image (about 376 KB) that LVGL draws directly. LVGL 8.4 can't scale an indexed image itself, and
 * letting it decode one allocates inside the draw; this allocates nothing.
 *
 * Scaling is bilinear on premultiplied colour (so rain edges don't darken), pixel-centre aligned like
 * Pillow's / LVGL 9's resize, in integer arithmetic.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_WIN_W    380       /* the window shown */
#define RADAR_WIN_H    330
#define RADAR_SRC_SIZE 384       /* RainViewer frame size asked for (384 px, zoom 6) */
/* the frame is shown scaled by RADAR_SCALE_NUM / RADAR_SCALE_DEN (4/3: it looks like a 512 px frame, the
 * same map scale as the old 256 px zoom 7 frames over four times the area) */
#define RADAR_SCALE_NUM 4
#define RADAR_SCALE_DEN 3
#define RADAR_PAL_MAX  256

/* The part of the source frame the window needs, bilinear taps included. */
typedef struct {
    int x0, y0, w, h;
} radar_crop_t;

/* For the RADAR_SRC_SIZE square frame and the RADAR_WIN_W x RADAR_WIN_H window centred on it:
 * {49, 68, 286, 248}. */
radar_crop_t radar_crop(void);

/* One packed frame: header, palette (straight, non-premultiplied ARGB8888 as 0xAARRGGBB), then w*h
 * palette indices, row by row. */
typedef struct {
    uint16_t src_w, src_h;       /* the frame it was cropped from (taps are clamped to it) */
    uint16_t w, h;               /* the crop */
    uint16_t npal;               /* entries used in pal[] */
    uint16_t reserved;
    uint32_t pal[RADAR_PAL_MAX];
    uint8_t idx[];               /* w * h */
} radar_pack_t;

/* Bytes to allocate for a packed frame of this crop. */
size_t radar_pack_bytes(const radar_crop_t *crop);

/* Packs the crop of an RGBA (R,G,B,A byte order, row-major, no padding) src_w x src_h image into *out
 * (radar_pack_bytes(crop) bytes). Every distinct colour gets a palette entry (in order of first appearance); once
 * there are RADAR_PAL_MAX, any further colour is mapped to the nearest kept one. Returns the number of pixels that had
 * to be mapped that way (0 = lossless), or -1 for bad arguments (crop not inside the image, null pointers). */
int radar_pack(const uint8_t *rgba, int src_w, int src_h, const radar_crop_t *crop, radar_pack_t *out);

/* The same, a row at a time, for use behind a streaming PNG decoder (ha_radar_png.h) so the whole image never exists:
 * begin, give it every row 0..src_h-1 in order (rows outside the crop are ignored), end. */
typedef struct {
    radar_pack_t *out;
    radar_crop_t crop;
    int src_w, src_h;
    int merged;                                   /* pixels mapped to the nearest colour */
    uint32_t last_argb;                           /* the last colour looked up, and its index */
    int last_idx;
    uint16_t hash[512];                           /* palette index + 1, 0 = empty slot */
} radar_pack_stream_t;

int radar_pack_begin(radar_pack_stream_t *ps, radar_pack_t *out, int src_w, int src_h, const radar_crop_t *crop);
void radar_pack_row(radar_pack_stream_t *ps, int y, const uint8_t *rgba_row);
int radar_pack_end(radar_pack_stream_t *ps);      /* the radar_pack() return value */

/* Working tables for radar_expand(), about 8 KB: far too much for a small task stack, so the caller
 * provides them (allocate one with the working image, in PSRAM, and keep it). */
typedef struct {
    int x0[RADAR_WIN_W], x1[RADAR_WIN_W], fx[RADAR_WIN_W];     /* per window column: the two source columns and the fraction */
    uint32_t pa[RADAR_PAL_MAX], pr[RADAR_PAL_MAX], pg[RADAR_PAL_MAX], pb[RADAR_PAL_MAX];   /* premultiplied palette */
    uint16_t c565[RADAR_PAL_MAX];      /* each palette colour as RGB565 (byte-swapped if asked) ... */
    uint8_t a8[RADAR_PAL_MAX];         /* ... and its alpha: what a pixel whose four neighbours agree becomes */
} radar_scratch_t;

/* Expands a packed frame into the working image for the RADAR_WIN_W x RADAR_WIN_H window: rgb565 gets
 * RADAR_WIN_W*RADAR_WIN_H uint16_t values (byte-swapped within each if swap_bytes, for LV_COLOR_16_SWAP) and
 * alpha the same number of bytes - LVGL's LV_IMG_CF_RGB565A8 layout is the colour plane followed by the alpha
 * plane, so a caller can allocate one block of 3 bytes per pixel and pass both pointers into it. The frame
 * must have been packed with radar_crop(). */
void radar_expand(const radar_pack_t *pack, radar_scratch_t *scratch, uint16_t *rgb565, uint8_t *alpha, bool swap_bytes);

#ifdef __cplusplus
}
#endif
