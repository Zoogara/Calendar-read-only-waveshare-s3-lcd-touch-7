/* Radar and satellite page (with-HA-weather only): opened from the radar icon on the weather page. The six
 * most recent RainViewer radar frames play as a loop over a base map, oldest to newest, with a progress bar and
 * the time of the frame on show; the latest Himawari infrared satellite image sits beside it. Everything comes
 * from Home Assistant over plain http: the radar template sensor (frame ids and times), and the files HA keeps
 * in /config/www (see Weather-Display's homeassistant/radar.yaml and satellite.yaml).
 *
 * Memory (measured on the display, see the commit history): about 1.07 MB of PSRAM while the page is open - the
 * six frames kept as lossless 8-bit indexed (ha_radar_pack.c, ~72 KB each), the satellite as RGB565 (251 KB), and one
 * shared 380x330 RGB565+alpha working image (376 KB) that each animation step is expanded into and LVGL draws
 * directly. Internal RAM: the objects below (a few hundred bytes), a mutex, and a parked task's control block.
 * The base map is a constant in flash. PNGs are decoded a row at a time (ha_radar_png.c, on the ROM's miniz): a file
 * and a 32 KB inflate window of PSRAM, where lodepng needed ~1.2 MB and failed when PSRAM was fragmented (error 83).
 *
 * Order: frames are only ever shown oldest to newest. They are sorted by their own times (never trusting HA's
 * order), the list shown is replaced atomically, and a refresh only swaps in a new list once the new frame has
 * arrived - so the loop never contains a newer frame before an older one, and a frame that can't be fetched
 * leaves the previous list playing.
 *
 * Refresh: frames arrive about a minute past each 10 minute mark, so while the page is open the loader checks at
 * 2 minutes past each mark and, if the new frame isn't there yet, every 45 s after that (ha_radar_data.h). It
 * fetches only frames it doesn't already hold. After the page is closed the data lingers for two minutes so
 * opening it again is instant, then everything is freed.
 *
 * Threads: the loader task (PSRAM stack: it only fetches, decodes and packs - no flash writes, no LVGL calls)
 * and the LVGL task (the animation timer, which expands the next frame under D->lock). */
#include "calendar_ui_internal.h"
#include "ha_radar_pack.h"
#include "ha_radar_data.h"
#include "ha_radar_png.h"
#include "time_fmt.h"
#include "ui_theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ui_radar";

#define RADAR_ENTITY     "sensor.template_weather_display_radar"
#define SAT_TIME_ENTITY  "sensor.himawari_latest"

#define BG_COLOR     0x000000
#define HEAD_COLOR   0xE8EAED
#define TEXT_COLOR   0xBDC1C6
#define BORDER_COLOR 0xB0B0B0
#define BAR_BG       0x303030
#define BAR_FG       0x2F8CD8

#define PANEL_W      (RADAR_WIN_W + 2)
#define PANEL_H      (RADAR_WIN_H + 2)
#define RADAR_X      14
#define SAT_X        404
#define PANEL_Y      71

/* A refresh holds the six frames on show and brings the new one in beside them, so there are spare slots; if it
 * needs more than are spare (a long gap), the frames that are dropping out are released first. */
#define RADAR_SLOTS      8
#define STEP_MS          700               /* as on the ESPHome page */
#define HOLD_STEPS       2                 /* the newest frame stays up for this many extra steps */
#define IDLE_LINGER_S    120               /* data kept this long after the page is closed */
#define MIN_INTERNAL_FREE 16000
#define MIN_PSRAM_LARGEST (1800 * 1024)    /* the resident block (~1.2 MB) is allocated first, and a PNG decode then needs two ~0.6 MB blocks */
#define HTTP_TIMEOUT_MS  15000             /* HA has been seen to stall for 18-26 s just as a new frame lands */
#define SENSOR_BUF       16384
#define PNG_BUF_MAX      262144

#ifdef HOST_RENDER
extern uint8_t radar_base_start[];       /* the screenshot renderer loads the fictional map into it */
#else
extern const uint8_t radar_base_start[] asm(RADAR_BASE_SYM);      /* set by CMakeLists.txt: radar_base.local.rgb565 if present */
#endif

/* ---------------------------------------------------------------------------------------------------------
 * Data, shared between the loader task and the UI
 * ------------------------------------------------------------------------------------------------------- */
typedef struct {
    SemaphoreHandle_t lock;                         /* guards n, ids, times, packs and the satellite copy */
    bool stop;                                      /* the page was closed (cleared if it is reopened) */
    int n;                                          /* frames ready, chronological: ids/times/packs[0..n-1] */
    char ids[RADAR_FRAMES][RADAR_ID_MAX];
    time_t times[RADAR_FRAMES];
    radar_pack_t *packs[RADAR_FRAMES];              /* each one of the slots below */
    uint8_t *res;                                   /* ONE block: work image, satellite, scratch and the pack slots - see new_data() */
    radar_pack_t *slot[RADAR_SLOTS];
    bool slot_used[RADAR_SLOTS];
    uint16_t *sat;                                  /* RADAR_WIN_W x RADAR_WIN_H RGB565, inside res */
    time_t sat_time;                                /* 0 = none yet */
    bool sat_dirty;
    char status[32];                                /* shown under the radar, right-aligned */
    char err[24];                                   /* the last thing that went wrong, shown with the status */
    uint8_t *work;                                  /* RGB565 plane then alpha plane: LV_IMG_CF_RGB565A8 */
    radar_scratch_t *scratch;
} radar_data_t;

static radar_data_t *D;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;     /* guards D and D->stop only */
static SemaphoreHandle_t s_wake;                              /* given when a loader session should start */
static TaskHandle_t s_loader;

/* ---------------------------------------------------------------------------------------------------------
 * Fetching
 * ------------------------------------------------------------------------------------------------------- */
static char *base_url(const app_settings_t *cfg, char *out, size_t sz)
{
    const char *b = cfg->ha_base_url;
    snprintf(out, sz, "%s%s", strstr(b, "://") ? "" : "http://", b);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
    return out;
}

/* GET url into a new PSRAM buffer of cap bytes (NUL-terminated). Returns the length, or -status / -1. */
static int http_get(const char *url, const char *bearer, uint8_t **out, size_t cap)
{
    *out = NULL;
    esp_http_client_config_t hc = {.url = url, .timeout_ms = HTTP_TIMEOUT_MS, .buffer_size = 1024, .buffer_size_tx = 768};
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (cl == NULL) {
        return -1;
    }
    if (bearer != NULL) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(cl, "Authorization", auth);
    }
    int result = -1;
    int64_t content_len = 0;
    if (esp_http_client_open(cl, 0) == ESP_OK && (content_len = esp_http_client_fetch_headers(cl)) >= 0) {
        int status = esp_http_client_get_status_code(cl);
        if (status == 200) {
            /* the buffer is the size of the file when HA says (it does for its static files), not a 256 KB guess held
             * through the decode: that guess is what fragmented PSRAM enough to fail a decode */
            if (content_len > 0 && (size_t)content_len < cap) {
                cap = (size_t)content_len;
            }
            uint8_t *buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            size_t total = 0;
            while (buf != NULL && total < cap) {
                int n = esp_http_client_read(cl, (char *)buf + total, (int)(cap - total));
                if (n <= 0) {
                    break;
                }
                total += (size_t)n;
            }
            if (buf != NULL && total > 0) {
                buf[total] = '\0';
                *out = buf;
                result = (int)total;
            } else {
                free(buf);
            }
        } else {
            result = -status;
        }
    }
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return result;
}

static void note_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note_err(const char *fmt, ...)
{
    if (D == NULL) return;
    char b[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    xSemaphoreTake(D->lock, portMAX_DELAY);
    snprintf(D->err, sizeof(D->err), "%s", b);
    xSemaphoreGive(D->lock);
}

static radar_pack_t *slot_take(void)
{
    radar_pack_t *r = NULL;
    xSemaphoreTake(D->lock, portMAX_DELAY);
    for (int i = 0; i < RADAR_SLOTS && r == NULL; i++) {
        if (!D->slot_used[i]) {
            D->slot_used[i] = true;
            r = D->slot[i];
        }
    }
    xSemaphoreGive(D->lock);
    return r;
}

static void slot_release(radar_pack_t *p)
{
    if (p == NULL) return;
    xSemaphoreTake(D->lock, portMAX_DELAY);
    for (int i = 0; i < RADAR_SLOTS; i++) {
        if (D->slot[i] == p) D->slot_used[i] = false;
    }
    xSemaphoreGive(D->lock);
}

static int slots_free(void)
{
    int f = 0;
    xSemaphoreTake(D->lock, portMAX_DELAY);
    for (int i = 0; i < RADAR_SLOTS; i++) f += D->slot_used[i] ? 0 : 1;
    xSemaphoreGive(D->lock);
    return f;
}

static void set_status(const char *s)
{
    if (D == NULL) return;
    xSemaphoreTake(D->lock, portMAX_DELAY);
    snprintf(D->status, sizeof(D->status), "%s", s);
    xSemaphoreGive(D->lock);
}

static bool stopping(void)
{
    bool s;
    taskENTER_CRITICAL(&s_mux);
    s = (D == NULL) || D->stop;
    taskEXIT_CRITICAL(&s_mux);
    return s;
}

typedef struct {
    char id[RADAR_ID_MAX];
    time_t time;
} listed_t;

/* Reads the radar sensor: up to RADAR_FRAMES frames, sorted oldest first and de-duplicated. Returns how many. */
static int read_frame_list(const char *base, const char *token, listed_t out[RADAR_FRAMES])
{
    char url[300];
    snprintf(url, sizeof(url), "%s/api/states/%s", base, RADAR_ENTITY);
    uint8_t *json = NULL;
    int len = http_get(url, token, &json, SENSOR_BUF);
    if (len <= 0) {
        ESP_LOGW(TAG, "radar sensor read failed (%d)", len);
        return 0;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)json, (size_t)len);
    cJSON *attrs = root ? cJSON_GetObjectItemCaseSensitive(root, "attributes") : NULL;
    char ids[RADAR_FRAMES][RADAR_ID_MAX];
    time_t times[RADAR_FRAMES];
    int n = 0;
    time_t now = time(NULL);
    for (int k = 1; k <= RADAR_FRAMES && attrs != NULL; k++) {
        char key[16];
        snprintf(key, sizeof(key), "url%d", k);
        cJSON *u = cJSON_GetObjectItemCaseSensitive(attrs, key);
        snprintf(key, sizeof(key), "timestamp%d", k);
        cJSON *t = cJSON_GetObjectItemCaseSensitive(attrs, key);
        int h, m;
        if (cJSON_IsString(u) && cJSON_IsString(t) && radar_id_from_url(u->valuestring, ids[n], sizeof(ids[n])) &&
            radar_parse_clock(t->valuestring, &h, &m)) {
            times[n] = radar_frame_time(now, h, m);
            n++;
        }
    }
    cJSON_Delete(root);
    free(json);
    int order[RADAR_FRAMES];
    int m = radar_order_frames(times, (const char(*)[RADAR_ID_MAX])ids, n, order);
    for (int i = 0; i < m; i++) {
        memcpy(out[i].id, ids[order[i]], RADAR_ID_MAX);
        out[i].time = times[order[i]];
    }
    return m;
}

static bool read_sat_time(const char *base, const char *token, time_t *out)
{
    char url[300];
    snprintf(url, sizeof(url), "%s/api/states/%s", base, SAT_TIME_ENTITY);
    uint8_t *json = NULL;
    int len = http_get(url, token, &json, 2048);
    if (len <= 0) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)json, (size_t)len);
    cJSON *st = root ? cJSON_GetObjectItemCaseSensitive(root, "state") : NULL;
    bool ok = cJSON_IsString(st) && radar_parse_iso_utc(st->valuestring, out);
    cJSON_Delete(root);
    free(json);
    return ok;
}

static int pack_row_cb(void *user, int y, const uint8_t *row)
{
    radar_pack_row((radar_pack_stream_t *)user, y, row);
    return !stopping();                                 /* the page was closed: stop decoding */
}

/* Fetch one radar frame and pack it straight from the PNG, a row at a time (ha_radar_png.c): no decoded image ever
 * exists, so a frame costs its file (~25 KB) and a 32 KB inflate window of PSRAM, not 1.2 MB. NULL on any failure. */
static radar_pack_t *fetch_frame(const char *base, const char *id)
{
    char url[300];
    snprintf(url, sizeof(url), "%s/local/radar/%s.png", base, id);
    uint8_t *png = NULL;
    int len = http_get(url, NULL, &png, PNG_BUF_MAX);
    if (len <= 0) {
        ESP_LOGW(TAG, "frame %s: fetch failed (%d)", id, len);
        note_err("fetch %d", len);
        return NULL;
    }
    int w = 0, h = 0;
    int rc = radar_png_info(png, (size_t)len, &w, &h);
    if (rc != RADAR_PNG_OK || w != RADAR_SRC_SIZE || h != RADAR_SRC_SIZE) {
        ESP_LOGW(TAG, "frame %s: not a %dx%d PNG we can read (%d, %dx%d)", id, RADAR_SRC_SIZE, RADAR_SRC_SIZE, rc, w, h);
        note_err("png %d", rc != RADAR_PNG_OK ? rc : 99);
        free(png);
        return NULL;
    }
    const radar_crop_t crop = radar_crop();
    radar_pack_t *p = slot_take();
    radar_pack_stream_t *ps = heap_caps_malloc(sizeof(*ps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL || ps == NULL || radar_pack_begin(ps, p, w, h, &crop) != 0) {
        note_err(p == NULL ? "no slot" : "no memory");
        slot_release(p);
        heap_caps_free(ps);
        free(png);
        return NULL;
    }
    rc = radar_png_rows(png, (size_t)len, pack_row_cb, ps);
    heap_caps_free(ps);
    free(png);
    if (rc != RADAR_PNG_OK) {
        ESP_LOGW(TAG, "frame %s: decode failed (%d)", id, rc);
        if (rc != RADAR_PNG_STOPPED) note_err("png %d", rc);
        slot_release(p);
        return NULL;
    }
    return p;
}

typedef struct {
    uint16_t *dst;
    int w;
} sat_conv_t;

static int sat_row_cb(void *user, int y, const uint8_t *row)
{
    sat_conv_t *c = (sat_conv_t *)user;
    uint16_t *d = c->dst + (size_t)y * c->w;
    for (int x = 0; x < c->w; x++) {
        const uint8_t *p = row + 4 * x;
        d[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
    }
    return !stopping();
}

/* Fetch the satellite image and convert it, row by row, straight into D->sat (the displayed buffer; at worst one refresh
 * shows it part-updated - it is never freed under LVGL's feet). */
static bool fetch_satellite(const char *base, time_t when)
{
    char url[300];
    snprintf(url, sizeof(url), "%s/local/satellite.png", base);
    uint8_t *png = NULL;
    int len = http_get(url, NULL, &png, PNG_BUF_MAX);
    if (len <= 0) {
        ESP_LOGW(TAG, "satellite: fetch failed (%d)", len);
        note_err("sat fetch %d", len);
        return false;
    }
    int w = 0, h = 0;
    int rc = radar_png_info(png, (size_t)len, &w, &h);
    if (rc != RADAR_PNG_OK || w != RADAR_WIN_W || h != RADAR_WIN_H) {
        ESP_LOGW(TAG, "satellite: not a %dx%d PNG we can read (%d, %dx%d)", RADAR_WIN_W, RADAR_WIN_H, rc, w, h);
        note_err("sat png %d", rc != RADAR_PNG_OK ? rc : 99);
        free(png);
        return false;
    }
    sat_conv_t conv = {.dst = D->sat, .w = w};
    rc = radar_png_rows(png, (size_t)len, sat_row_cb, &conv);
    free(png);
    if (rc != RADAR_PNG_OK) {
        ESP_LOGW(TAG, "satellite: decode failed (%d)", rc);
        note_err("sat png %d", rc);
        return false;
    }
    xSemaphoreTake(D->lock, portMAX_DELAY);
    D->sat_time = when;
    D->sat_dirty = true;
    xSemaphoreGive(D->lock);
    return true;
}

/* One check: bring the radar frames and the satellite up to date. Returns true if everything the sensor listed
 * is now held (so there is nothing to retry), false if the newest frame (or the sensor) wasn't available. */
static bool refresh_once(const app_settings_t *cfg)
{
    char base[160];
    base_url(cfg, base, sizeof(base));
    listed_t list[RADAR_FRAMES];
    int n = read_frame_list(base, cfg->ha_token, list);
    if (n == 0) {
        set_status(D->n == 0 ? "No radar data" : "");
        return false;
    }
    char new_ids[RADAR_FRAMES][RADAR_ID_MAX];
    for (int i = 0; i < n; i++) memcpy(new_ids[i], list[i].id, RADAR_ID_MAX);

    xSemaphoreTake(D->lock, portMAX_DELAY);
    int n_old = D->n;
    char old_ids[RADAR_FRAMES][RADAR_ID_MAX];
    memcpy(old_ids, D->ids, sizeof(old_ids));
    radar_pack_t *old_packs[RADAR_FRAMES];
    memcpy(old_packs, D->packs, sizeof(old_packs));
    xSemaphoreGive(D->lock);

    int reuse[RADAR_FRAMES];
    int to_fetch = radar_plan_frames(old_ids, n_old, new_ids, n, reuse);
    bool complete = true;

    if (n_old > 0 && to_fetch > slots_free()) {
        /* more new frames than spare slots (a long gap): first release the held frames that are dropping out, by
         * swapping in a list of just the ones that stay - still chronological, just shorter - then fetch the rest */
        bool stays[RADAR_FRAMES] = {false};
        for (int i = 0; i < n; i++) if (reuse[i] >= 0) stays[reuse[i]] = true;
        xSemaphoreTake(D->lock, portMAX_DELAY);
        radar_pack_t *drop[RADAR_FRAMES];
        int nd = 0, k = 0;
        for (int j = 0; j < n_old; j++) {
            if (stays[j]) {
                if (k != j) {
                    memcpy(D->ids[k], D->ids[j], RADAR_ID_MAX);
                    D->times[k] = D->times[j];
                    D->packs[k] = D->packs[j];
                }
                k++;
            } else {
                drop[nd++] = D->packs[j];
            }
        }
        D->n = k;
        xSemaphoreGive(D->lock);
        for (int j = 0; j < nd; j++) slot_release(drop[j]);
        n_old = k;
        memcpy(old_ids, D->ids, sizeof(old_ids));
        memcpy(old_packs, D->packs, sizeof(old_packs));
        to_fetch = radar_plan_frames(old_ids, n_old, new_ids, n, reuse);
    }

    if (to_fetch > 0) {
        radar_pack_t *np[RADAR_FRAMES] = {0};
        int got = 0;
        int i = 0;
        for (; i < n && !stopping(); i++) {
            if (reuse[i] >= 0) {
                np[i] = old_packs[reuse[i]];
                continue;
            }
            char st[32];
            snprintf(st, sizeof(st), "Loading %d/%d", got + 1, to_fetch);
            set_status(st);
            np[i] = fetch_frame(base, list[i].id);
            if (np[i] == NULL) {
                complete = false;
            } else {
                got++;
            }
            if (n_old == 0 && np[i] != NULL) {
                /* first load: show frames as they arrive. Frames come oldest first, so what is on show is always a
                 * chronological prefix; a failed frame is simply skipped (a gap, never disorder). */
                xSemaphoreTake(D->lock, portMAX_DELAY);
                int k = D->n;
                memcpy(D->ids[k], list[i].id, RADAR_ID_MAX);
                D->times[k] = list[i].time;
                D->packs[k] = np[i];
                D->n = k + 1;
                xSemaphoreGive(D->lock);
            }
        }
        if (i < n) {
            complete = false;                 /* stopped part-way (the page was closed): not everything is held */
        }
        if (n_old > 0) {
            /* a refresh: the newest frame (or all new ones) must have arrived before anything changes, so the loop
             * on show is never a mixture. Frames that failed are dropped from the new list, but if the newest
             * one failed we keep the old list and try again soon. */
            bool newest_ok = np[n - 1] != NULL;
            if (newest_ok && !stopping()) {
                xSemaphoreTake(D->lock, portMAX_DELAY);
                radar_pack_t *dropped[RADAR_FRAMES];
                int nd = 0;
                for (int j = 0; j < n_old; j++) {
                    bool kept = false;
                    for (int i = 0; i < n; i++) kept = kept || np[i] == D->packs[j];
                    if (!kept) dropped[nd++] = D->packs[j];
                }
                int k = 0;
                for (int i = 0; i < n; i++) {
                    if (np[i] == NULL) continue;
                    memcpy(D->ids[k], list[i].id, RADAR_ID_MAX);
                    D->times[k] = list[i].time;
                    D->packs[k] = np[i];
                    k++;
                }
                D->n = k;
                xSemaphoreGive(D->lock);
                for (int j = 0; j < nd; j++) slot_release(dropped[j]);       /* after the swap: the UI can't be using them */
            } else {
                for (int i = 0; i < n; i++) {
                    bool held = false;
                    for (int j = 0; j < n_old; j++) held = held || np[i] == old_packs[j];
                    if (np[i] != NULL && !held) slot_release(np[i]);          /* fetched, but the list isn't being replaced */
                }
                complete = false;
            }
        }
        set_status("");
    }
    {
        char st[48] = "";
        xSemaphoreTake(D->lock, portMAX_DELAY);
        int held = D->n;
        xSemaphoreGive(D->lock);
        if (held < n) {
            xSemaphoreTake(D->lock, portMAX_DELAY);
            snprintf(st, sizeof(st), "%d/%d %s", held, n, D->err);
            xSemaphoreGive(D->lock);
            complete = false;
        } else {
            xSemaphoreTake(D->lock, portMAX_DELAY);
            D->err[0] = '\0';
            xSemaphoreGive(D->lock);
        }
        set_status(st);
    }

    /* the satellite: when HA's image is newer than the one held */
    time_t st = 0;
    if (!stopping() && read_sat_time(base, cfg->ha_token, &st)) {
        bool newer;
        xSemaphoreTake(D->lock, portMAX_DELAY);
        newer = st != D->sat_time;
        xSemaphoreGive(D->lock);
        if (newer) {
            fetch_satellite(base, st);
        }
    }
    return complete;
}

static void free_data(radar_data_t *d)
{
    free(d->res);
    vSemaphoreDelete(d->lock);
    free(d);
}

static void loader_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        const app_settings_t *cfg = ui_get_cfg();
        int retries = 0;
        int64_t linger_start = 0;
        time_t next = 0;                      /* 0 = check straight away (the first load) */
        for (;;) {
            bool closing;
            taskENTER_CRITICAL(&s_mux);
            closing = D->stop;
            taskEXIT_CRITICAL(&s_mux);
            if (closing) {
                int64_t nowu = esp_timer_get_time();
                if (linger_start == 0) {
                    linger_start = nowu;
                    ESP_LOGI(TAG, "page closed; keeping the frames for %d s", IDLE_LINGER_S);
                }
                if (nowu - linger_start > (int64_t)IDLE_LINGER_S * 1000000) {
                    radar_data_t *d = NULL;
                    taskENTER_CRITICAL(&s_mux);
                    if (D->stop) {            /* still closed: free it. (If reopened meanwhile, carry on.) */
                        d = D;
                        D = NULL;
                    }
                    taskEXIT_CRITICAL(&s_mux);
                    if (d != NULL) {
                        free_data(d);
                        ESP_LOGI(TAG, "radar data freed; psram free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (linger_start != 0) {
                linger_start = 0;
                next = 0;                     /* reopened: look straight away (cheap when nothing is new) */
                retries = 0;
            }
            time_t now = time(NULL);
            if (next == 0 || now >= next) {
                bool complete = refresh_once(cfg);
                now = time(NULL);
                if (complete) {
                    retries = 0;
                    next = radar_next_check(now);
                } else if (++retries <= RADAR_RETRIES) {
                    next = now + RADAR_RETRY_S;
                } else {
                    retries = 0;
                    next = radar_next_check(now);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * The page
 * ------------------------------------------------------------------------------------------------------- */
static lv_obj_t *s_overlay;
static lv_obj_t *s_img_radar, *s_img_sat;
static lv_obj_t *s_bar, *s_radar_ts, *s_sat_ts, *s_status;
static lv_timer_t *s_timer;
static lv_img_dsc_t s_dsc_radar, s_dsc_sat;
static const lv_img_dsc_t s_dsc_base = {
    .header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = RADAR_WIN_W, .h = RADAR_WIN_H},
    .data_size = RADAR_WIN_W * RADAR_WIN_H * 2,
    .data = radar_base_start,
};
static char s_radar_ts_buf[48], s_sat_ts_buf[48], s_status_buf[32];
static int s_step;
static int64_t s_expand_us_total;
static int s_expand_count;
static time_t s_shown_sat_time;

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void format_when(time_t t, char *out, size_t sz)
{
    struct tm lt;
    localtime_r(&t, &lt);
    char date[24], hm[12];
    strftime(date, sizeof(date), "%a %d %b", &lt);
    tfmt_hm(ui_get_cfg()->clock_24h, lt.tm_hour, lt.tm_min, hm, sizeof(hm));
    snprintf(out, sz, "%s %s", date, hm);
}

static lv_obj_t *add_panel(lv_obj_t *parent, int x)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, PANEL_Y);
    lv_obj_set_size(p, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(p, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(BORDER_COLOR), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static void animate_cb(lv_timer_t *t)
{
    if (D == NULL || s_overlay == NULL) {
        return;
    }
    if (xSemaphoreTake(D->lock, 0) != pdTRUE) {
        return;                                    /* the loader is swapping the list: skip this step */
    }
    int n = D->n;
    bool sat_dirty = D->sat_dirty, status_changed = strcmp(s_status_buf, D->status) != 0;
    time_t sat_time = D->sat_time;
    if (status_changed) {
        snprintf(s_status_buf, sizeof(s_status_buf), "%s", D->status);
    }
    if (n > 0) {
        int cycle = n + HOLD_STEPS;
        if (s_step >= cycle) s_step = 0;
        int k = s_step < n ? s_step : n - 1;     /* oldest to newest, then the newest held */
        int64_t a = esp_timer_get_time();
        radar_expand(D->packs[k], D->scratch, (uint16_t *)D->work, D->work + (size_t)RADAR_WIN_W * RADAR_WIN_H * 2, false);
        s_expand_us_total += esp_timer_get_time() - a;
        s_expand_count++;
        format_when(D->times[k], s_radar_ts_buf, sizeof(s_radar_ts_buf));
        lv_obj_invalidate(s_img_radar);
        lv_label_set_text_static(s_radar_ts, s_radar_ts_buf);
        lv_bar_set_range(s_bar, 0, RADAR_FRAMES);
        lv_bar_set_value(s_bar, k + 1, LV_ANIM_OFF);
        s_step++;
        if (s_step >= cycle) {
            s_step = 0;
            if (s_expand_count > 0) {
                ESP_LOGI(TAG, "loop of %d frames: expand averaged %lld us", n, (long long)(s_expand_us_total / s_expand_count));
            }
            s_expand_us_total = 0;
            s_expand_count = 0;
        }
    }
    if (sat_dirty || sat_time != s_shown_sat_time) {
        D->sat_dirty = false;
        s_shown_sat_time = sat_time;
        if (sat_time != 0) {
            format_when(sat_time, s_sat_ts_buf, sizeof(s_sat_ts_buf));
        }
        lv_label_set_text_static(s_sat_ts, s_sat_ts_buf);
        lv_obj_invalidate(s_img_sat);
    }
    xSemaphoreGive(D->lock);
    if (status_changed) {
        lv_label_set_text_static(s_status, s_status_buf);
    }
}

static void tap_cb(lv_event_t *e)
{
    /* the closing tap's release mustn't land on the weather page underneath and count as a tap there */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ui_radar_close();
}

bool ui_radar_is_open(void)
{
    return s_overlay != NULL;
}

/* A short-lived note on the top layer, for when the page can't open (a tap that does nothing looks broken). */
static lv_obj_t *s_note;
static lv_timer_t *s_note_timer;

static void note_gone_cb(lv_timer_t *t)
{
    if (s_note != NULL) {
        lv_obj_del(s_note);
        s_note = NULL;
    }
    lv_timer_del(t);
    s_note_timer = NULL;
}

static void show_note(const char *text)
{
    if (s_note != NULL) {
        lv_obj_del(s_note);
        s_note = NULL;
    }
    if (s_note_timer != NULL) {
        lv_timer_del(s_note_timer);
        s_note_timer = NULL;
    }
    s_note = lv_label_create(lv_layer_top());
    lv_label_set_text(s_note, text);
    lv_obj_set_style_text_font(s_note, &gcal_font_20, 0);
    lv_obj_set_style_text_color(s_note, lv_color_hex(0xF9AB00), 0);
    lv_obj_set_style_bg_color(s_note, lv_color_hex(0x202124), 0);
    lv_obj_set_style_bg_opa(s_note, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_note, 10, 0);
    lv_obj_align(s_note, LV_ALIGN_BOTTOM_MID, 0, -30);
    s_note_timer = lv_timer_create(note_gone_cb, 3000, NULL);
}

void ui_radar_close(void)
{
    if (s_overlay == NULL) {
        return;
    }
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    s_img_radar = s_img_sat = s_bar = s_radar_ts = s_sat_ts = s_status = NULL;
    taskENTER_CRITICAL(&s_mux);
    if (D != NULL) {
        D->stop = true;                  /* the loader frees everything after IDLE_LINGER_S */
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* All of the page's long-lived memory in ONE PSRAM block: the working image, the satellite, the scratch tables and the
 * pack slots. Allocated piecemeal (and with a decode's two ~0.6 MB buffers coming and going between) it fragmented PSRAM
 * until a decode failed with lodepng's "memory allocation failed" (error 83); as one block, what is left stays in one
 * piece for the decodes. */
static radar_data_t *new_data(void)
{
    radar_data_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d == NULL) return NULL;
    const radar_crop_t crop = radar_crop();
    const size_t px = (size_t)RADAR_WIN_W * RADAR_WIN_H;
    const size_t work_b = (px * 3 + 15) & ~(size_t)15, sat_b = (px * 2 + 15) & ~(size_t)15;
    const size_t scratch_b = (sizeof(radar_scratch_t) + 15) & ~(size_t)15, slot_b = (radar_pack_bytes(&crop) + 15) & ~(size_t)15;
    d->lock = xSemaphoreCreateMutex();
    d->res = heap_caps_calloc(1, work_b + sat_b + scratch_b + slot_b * RADAR_SLOTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d->lock == NULL || d->res == NULL) {
        if (d->lock) vSemaphoreDelete(d->lock);
        free(d->res);
        free(d);
        return NULL;
    }
    d->work = d->res;
    d->sat = (uint16_t *)(d->res + work_b);
    d->scratch = (radar_scratch_t *)(d->res + work_b + sat_b);
    for (int i = 0; i < RADAR_SLOTS; i++) {
        d->slot[i] = (radar_pack_t *)(d->res + work_b + sat_b + scratch_b + slot_b * (size_t)i);
    }
    ESP_LOGI(TAG, "resident block %u bytes", (unsigned)(work_b + sat_b + scratch_b + slot_b * RADAR_SLOTS));
    return d;
}

void ui_radar_init(void)
{
    s_wake = xSemaphoreCreateBinary();
    if (s_wake == NULL ||
        xTaskCreateWithCaps(loader_task, "radar_load", 12288, NULL, 3, &s_loader, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "could not start the radar loader task");
        s_loader = NULL;
    }
}

void ui_radar_open(void)
{
#ifndef HOST_RENDER
    if (s_overlay != NULL || s_loader == NULL) {
        return;
    }
#else
    if (s_overlay != NULL) {
        return;
    }
#endif
    const app_settings_t *cfg = ui_get_cfg();
#ifndef HOST_RENDER
    if (cfg->ha_base_url[0] == '\0' || cfg->ha_token[0] == '\0') {
        ESP_LOGW(TAG, "no Home Assistant address/token set - no radar");
        show_note("Radar needs the Home Assistant address and token");
        return;
    }
#endif
    /* Reuse data still held from a page closed in the last two minutes; otherwise start fresh. */
    bool reused = false;
    taskENTER_CRITICAL(&s_mux);
    if (D != NULL) {
        D->stop = false;
        reused = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!reused) {
        /* New data needs ~1 MB of PSRAM, and a PNG decode ~0.85 MB in one piece: check before building anything. (Data
         * still held from a page closed in the last two minutes needs no new allocation, so isn't checked - and with
         * ~1 MB of it resident the largest free block is legitimately smaller.) */
        size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "opening: internal %u free / %u largest, psram %u free / %u largest", (unsigned)int_free,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)ps_largest);
        if (int_free < MIN_INTERNAL_FREE || ps_largest < MIN_PSRAM_LARGEST) {
            ESP_LOGW(TAG, "not enough free memory just now - radar not opened");
            show_note("Not enough free memory for the radar just now");
            return;
        }
        radar_data_t *d = new_data();
        if (d == NULL) {
            ESP_LOGW(TAG, "no memory for the radar data - radar not opened");
            show_note("Not enough free memory for the radar just now");
            return;
        }
        bool installed = false;
        taskENTER_CRITICAL(&s_mux);
        if (D == NULL) {
            D = d;
            installed = true;
        } else {
            D->stop = false;                 /* a session appeared meanwhile: use it */
        }
        taskEXIT_CRITICAL(&s_mux);
        if (installed) {
            xSemaphoreGive(s_wake);
        } else {
            free_data(d);
        }
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, tap_cb, LV_EVENT_CLICKED, NULL);

    const char *place = cfg->weather_place;
    char title[96];
    if (place[0] != '\0') {
        snprintf(title, sizeof(title), "%s radar", place);
    } else {
        snprintf(title, sizeof(title), "Radar");
    }
    lv_obj_t *t = add_label(s_overlay, &gcal_font_20, HEAD_COLOR, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 15, 10);
    lv_obj_t *h1 = add_label(s_overlay, &gcal_font_20, HEAD_COLOR, "Radar");
    lv_obj_set_pos(h1, RADAR_X + 1, 40);
    lv_obj_t *h2 = add_label(s_overlay, &gcal_font_20, HEAD_COLOR, "Satellite");
    lv_obj_set_pos(h2, SAT_X + 1, 40);

    lv_obj_t *rp = add_panel(s_overlay, RADAR_X);
    lv_obj_t *base = lv_img_create(rp);
    lv_img_set_src(base, &s_dsc_base);
    lv_obj_set_pos(base, 0, 0);
    memset(D->work, 0, (size_t)RADAR_WIN_W * RADAR_WIN_H * 3);
    s_dsc_radar.header.always_zero = 0;
    s_dsc_radar.header.cf = LV_IMG_CF_RGB565A8;
    s_dsc_radar.header.w = RADAR_WIN_W;
    s_dsc_radar.header.h = RADAR_WIN_H;
    s_dsc_radar.data_size = RADAR_WIN_W * RADAR_WIN_H * 3;
    s_dsc_radar.data = D->work;
    s_img_radar = lv_img_create(rp);
    lv_img_set_src(s_img_radar, &s_dsc_radar);
    lv_obj_set_pos(s_img_radar, 0, 0);

    lv_obj_t *sp = add_panel(s_overlay, SAT_X);
    s_dsc_sat.header.always_zero = 0;
    s_dsc_sat.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc_sat.header.w = RADAR_WIN_W;
    s_dsc_sat.header.h = RADAR_WIN_H;
    s_dsc_sat.data_size = RADAR_WIN_W * RADAR_WIN_H * 2;
    s_dsc_sat.data = (const uint8_t *)D->sat;
    s_img_sat = lv_img_create(sp);
    lv_img_set_src(s_img_sat, &s_dsc_sat);
    lv_obj_set_pos(s_img_sat, 0, 0);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_pos(s_bar, RADAR_X + 1, PANEL_Y + PANEL_H + 2);
    lv_obj_set_size(s_bar, RADAR_WIN_W, 6);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BAR_FG), LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, RADAR_FRAMES);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    int label_y = PANEL_Y + PANEL_H + 12;
    s_radar_ts_buf[0] = '\0';
    s_radar_ts = add_label(s_overlay, &gcal_font_14, TEXT_COLOR, "");
    lv_label_set_text_static(s_radar_ts, s_radar_ts_buf);
    lv_obj_set_pos(s_radar_ts, RADAR_X + 1, label_y);
    s_status_buf[0] = '\0';
    s_status = add_label(s_overlay, &gcal_font_14, TEXT_COLOR, "");
    lv_label_set_text_static(s_status, s_status_buf);
    lv_obj_set_width(s_status, 200);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_status, RADAR_X + 1 + RADAR_WIN_W - 200, label_y);
    snprintf(s_sat_ts_buf, sizeof(s_sat_ts_buf), "%s", "");
    s_sat_ts = add_label(s_overlay, &gcal_font_14, TEXT_COLOR, "");
    lv_label_set_text_static(s_sat_ts, s_sat_ts_buf);
    lv_obj_set_pos(s_sat_ts, SAT_X + 1, label_y);
    s_shown_sat_time = (time_t)-1;           /* force the satellite label to be written on the first step */

    s_step = 0;
    s_expand_us_total = 0;
    s_expand_count = 0;
    s_timer = lv_timer_create(animate_cb, STEP_MS, NULL);
    animate_cb(NULL);

    /* The opening tap's release mustn't land on the page and close it. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
}

#ifdef HOST_RENDER
/* For tools/screenshots: puts already-built demo frames and satellite into the page's data, as the loader would have, so
 * the real page can be rendered with fictional data. (The loader task never runs in the renderer.) */
void ui_radar_host_load(const radar_pack_t *const *packs, const time_t *times, int n, const uint16_t *sat, time_t sat_time)
{
    if (D == NULL) {
        D = new_data();
    }
    D->stop = false;
    D->n = 0;
    const radar_crop_t crop = radar_crop();
    for (int i = 0; i < n && i < RADAR_FRAMES; i++) {
        radar_pack_t *slot = slot_take();
        memcpy(slot, packs[i], radar_pack_bytes(&crop));
        D->packs[i] = slot;
        D->times[i] = times[i];
        D->n++;
    }
    memcpy(D->sat, sat, (size_t)RADAR_WIN_W * RADAR_WIN_H * 2);
    D->sat_time = sat_time;
    D->sat_dirty = true;
}

/* One animation step, on demand, for a given frame (the renderer has no timer running). */
void ui_radar_host_step(int step)
{
    s_step = step;
    animate_cb(NULL);
}
#endif
