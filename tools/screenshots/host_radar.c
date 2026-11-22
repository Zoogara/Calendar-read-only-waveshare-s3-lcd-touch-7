/* The radar and satellite page for the renderer: the real ui_radar.c, with fictional data. Its loader task never runs
 * here (no network); ui_radar_host_load() puts the frames and the satellite in the page's data as the loader would have.
 * Everything drawn is invented: the rain field, the satellite image and (components/calendar_ui/radar_base.rgb565) the
 * map. The colours are RainViewer's published rain palette. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "calendar_ui_internal.h"
#include "ha_radar_pack.h"
#include "ha_radar_png.h"
#include "ha_radar_data.h"
#include "cJSON.h"
#include "demo.h"

/* ---- stand-ins for what ui_radar.c's loader links against (it never runs here) ---- */
cJSON *cJSON_ParseWithLength(const char *v, size_t n) { (void)v; (void)n; return NULL; }
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *o, const char *k) { (void)o; (void)k; return NULL; }
int cJSON_IsString(const cJSON *i) { (void)i; return 0; }
void cJSON_Delete(cJSON *c) { (void)c; }
int radar_zlib_inflate(const uint8_t *in, size_t n, radar_put_fn put, void *user) { (void)in; (void)n; (void)put; (void)user; return 0; }

/* the page's base map: ui_radar.c reads it from here when built for the host */
uint8_t radar_base_start[3][RADAR_WIN_W * RADAR_WIN_H * 2];      /* zoom 5, 6, 7 */

void ui_radar_host_load(const radar_pack_t *const *packs, const time_t *times, int n, const uint16_t *sat, time_t sat_time);
void ui_radar_host_step(int step);
void ui_radar_host_zoom(int zoom);

/* RainViewer's rain palette, weakest to heaviest: the tan of light rain (alpha rising), light to dark blue, then yellow to red */
static const uint8_t RAMP[][4] = {
    {99,97,89,20},{102,99,90,25},{105,102,92,30},{108,104,93,36},{111,107,95,41},{114,110,97,46},{117,112,98,52},{120,115,100,57},
    {124,117,101,62},{127,120,103,68},{130,123,105,73},{133,125,106,78},{136,128,108,84},{139,130,109,89},{142,133,111,94},
    {146,136,113,100},{158,147,117,110},{170,158,121,120},{182,169,126,130},{194,180,130,140},{206,192,135,150},{210,196,139,160},
    {214,200,143,170},{218,204,147,180},{222,208,151,190},
    {136,221,238,255},{108,209,235,255},{81,197,232,255},{54,186,229,255},{27,174,226,255},{0,163,224,255},{0,154,213,255},
    {0,145,202,255},{0,136,191,255},{0,127,180,255},{0,119,170,255},{0,112,163,255},{0,105,156,255},{0,98,149,255},{0,91,142,255},
    {0,85,136,255},{0,81,128,255},{0,78,120,255},{0,74,112,255},{0,71,104,255},
    {255,238,0,255},{255,224,0,255},{255,210,0,255},{255,197,0,255},{255,183,0,255},{255,159,0,255},{255,68,0,255},
};
#define RAMP_N ((int)(sizeof(RAMP) / sizeof(RAMP[0])))

static float noise2(int x, int y, int seed)
{
    unsigned h = (unsigned)(x * 374761393 + y * 668265263 + seed * 2147483647u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float vnoise(float x, float y, int seed)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = noise2(xi, yi, seed), b = noise2(xi + 1, yi, seed), c = noise2(xi, yi + 1, seed), d = noise2(xi + 1, yi + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static float fbm(float x, float y, int seed)
{
    float v = 0, amp = 0.5f;
    for (int o = 0; o < 4; o++, amp *= 0.5f, x *= 2, y *= 2) v += amp * vnoise(x, y, seed + o);
    return v;
}

/* a 384x384 rain frame: a broad band of light rain crossing from the north-west, with heavier cells drifting east */
static void make_frame(uint8_t *rgba, int f)
{
    for (int y = 0; y < RADAR_SRC_SIZE; y++) {
        for (int x = 0; x < RADAR_SRC_SIZE; x++) {
            float fx = (float)x - 14.0f * f, fy = (float)y;                  /* the field drifts east, 14 px a frame */
            float band = 0.55f - fabsf((fy - 0.55f * (fx - 40.0f)) - 40.0f) / 150.0f;     /* a diagonal band */
            float cells = 0;
            static const float C[5][4] = {{150, 150, 34, 0.55f}, {232, 238, 26, 0.62f}, {96, 262, 30, 0.50f}, {290, 130, 22, 0.38f}, {44, 190, 28, 0.42f}};
            for (int i = 0; i < 5; i++) {
                float dx = fx - C[i][0], dy = fy - C[i][1];
                cells += C[i][3] * expf(-(dx * dx + dy * dy) / (2 * C[i][2] * C[i][2]));
            }
            float v = (band > 0 ? band * 1.0f : 0) * (0.45f + 0.9f * fbm(fx * 0.03f, fy * 0.03f, 7)) + cells * (0.5f + 0.7f * fbm(fx * 0.05f, fy * 0.05f, 3));
            uint8_t *p = rgba + ((size_t)y * RADAR_SRC_SIZE + x) * 4;
            if (v < 0.12f) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            int idx = (int)((v - 0.12f) / 0.72f * (RAMP_N - 1));
            if (idx > RAMP_N - 1) idx = RAMP_N - 1;
            memcpy(p, RAMP[idx], 4);
        }
    }
}

/* an infrared satellite picture: grey cloud with the coldest tops coloured, a faint coastline */
static void make_satellite(uint16_t *out)
{
    for (int y = 0; y < RADAR_WIN_H; y++) {
        for (int x = 0; x < RADAR_WIN_W; x++) {
            float n = fbm(x * 0.012f, y * 0.012f, 21) * 1.05f + 0.2f * fbm(x * 0.05f, y * 0.05f, 5) - 0.1f;
            n += 0.22f * expf(-(((x - 150) * (x - 150)) / 9000.0f + ((y - 120) * (y - 120)) / 5000.0f));
            if (n < 0) n = 0;
            if (n > 1) n = 1;
            int r, g, b;
            if (n < 0.68f) {                                   /* land and thin cloud: dark to light grey */
                int v = (int)(55 + n / 0.68f * 160);
                r = g = b = v;
            } else {                                           /* cold tops: cyan, blue, green, yellow */
                float t = (n - 0.68f) / 0.32f;
                if (t < 0.3f) { r = 120 - (int)(t * 300); g = 215; b = 245; }
                else if (t < 0.6f) { r = 20; g = 215 - (int)((t - 0.3f) * 300); b = 245 - (int)((t - 0.3f) * 300); }
                else { r = 40 + (int)((t - 0.6f) * 500); g = 190 + (int)((t - 0.6f) * 150); b = 60; }
            }
            float coast = fabsf((float)y - (210.0f + 40.0f * sinf(x * 0.021f) + 0.35f * x));
            if (coast < 1.0f) { r = r / 2 + 40; g = g / 2 + 40; b = b / 2 + 40; }
            out[(size_t)y * RADAR_WIN_W + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

void host_radar_zoom(int zoom)
{
    ui_radar_host_zoom(zoom);
}

void host_radar_show(const char *base_map_dir)
{
    for (int z = 5; z <= 7; z++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/radar_base_z%d.rgb565", base_map_dir, z);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            size_t got = fread(radar_base_start[z - 5], 1, sizeof(radar_base_start[0]), f);
            (void)got;
            fclose(f);
        }
    }
    const radar_crop_t crop = radar_crop();
    uint8_t *rgba = malloc((size_t)RADAR_SRC_SIZE * RADAR_SRC_SIZE * 4);
    radar_pack_t *packs[RADAR_FRAMES];
    time_t times[RADAR_FRAMES];
    time_t now = demo_now();
    for (int i = 0; i < RADAR_FRAMES; i++) {
        make_frame(rgba, i);
        packs[i] = malloc(radar_pack_bytes(&crop));
        radar_pack(rgba, RADAR_SRC_SIZE, RADAR_SRC_SIZE, &crop, packs[i]);
        times[i] = now - (time_t)(RADAR_FRAMES - 1 - i) * 600 - 4 * 60;     /* the newest frame is a few minutes old */
    }
    uint16_t *sat = malloc((size_t)RADAR_WIN_W * RADAR_WIN_H * 2);
    make_satellite(sat);
    ui_radar_host_load((const radar_pack_t *const *)packs, times, RADAR_FRAMES, sat, now - 52 * 60);
    ui_radar_open();
    ui_radar_host_step(RADAR_FRAMES - 1);                                    /* show the newest frame */
    free(rgba);
    free(sat);
    for (int i = 0; i < RADAR_FRAMES; i++) free(packs[i]);
}
