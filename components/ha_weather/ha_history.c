/* Last 24 hours of one Home Assistant sensor, for the graph at the bottom of
 * the weather details panel (calendar_ui/ui_weather.c).
 *
 * Uses the REST history API with minimal_response - one small object per
 * state change, ~75 bytes each, 300-400 a day for a temperature sensor - and
 * resamples it onto HA_HISTORY_POINTS evenly spaced steps ending now (each
 * step holds the last known value, as the sensor's state does). The reply is
 * scanned for "state" / "last_changed" pairs rather than parsed as JSON, so a
 * very chatty sensor whose reply hits the size cap still gives a graph of what
 * arrived, and nothing big is built in memory. Plain http:// on the LAN only,
 * like the rest of ha_weather. */
#include "ha_weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ha_history";

#define HTTP_TIMEOUT_MS  15000
#define MAX_BYTES        (384 * 1024)
#define REFRESH_US       (10LL * 60 * 1000000)
#define RETRY_US         (3LL * 60 * 1000000)
#define STALE_AFTER_US   (60LL * 60 * 1000000)

static SemaphoreHandle_t s_lock;
static ha_history_t *s_cur;             /* PSRAM; NULL until the first success */
static int64_t s_ok_us = -1;
static int64_t s_next_try_us;
static char s_cur_entity[HA_HISTORY_SERIES][64];

struct resp_buf {
    char *data;
    size_t len, cap;
    bool full;
};

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    struct resp_buf *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && !b->full) {
        size_t take = evt->data_len;
        if (b->len + take > MAX_BYTES) {
            take = MAX_BYTES - b->len;
            b->full = true;
        }
        if (b->len + take + 1 > b->cap) {
            size_t cap = (b->len + take + 1) * 2;
            if (cap > MAX_BYTES + 1) {
                cap = MAX_BYTES + 1;
            }
            char *g = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM);
            if (g == NULL) {
                return ESP_FAIL;
            }
            b->data = g;
            b->cap = cap;
        }
        memcpy(b->data + b->len, evt->data, take);
        b->len += take;
        b->data[b->len] = '\0';
    }
    return ESP_OK;
}

/* GET url with the bearer token into a PSRAM buffer; caller frees *out. */
static esp_err_t http_get(const app_settings_t *cfg, const char *url, struct resp_buf *resp, int *status)
{
    memset(resp, 0, sizeof(*resp));
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_data,
        .user_data = resp,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char auth[APP_SETTINGS_MAX_HA_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->ha_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(auth, 0, sizeof(auth));
    return err;
}

/* "2026-10-18T08:45:56.580037+11:00" / "...Z" -> UTC epoch, or -1. */
static time_t parse_iso(const char *s)
{
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) {
        return -1;
    }
    struct tm tm = {.tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D,
                    .tm_hour = h, .tm_min = m, .tm_sec = sec};
    time_t t = (time_t)0;
    /* days from civil, avoiding timegm() / the TZ environment */
    int y = Y - (M <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    (void)tm;
    t = (time_t)(days * 86400L + h * 3600L + m * 60L + sec);
    /* the offset after the seconds (and optional fraction) */
    const char *p = s + 19;
    if (*p == '.') {
        while (*++p >= '0' && *p <= '9') {
        }
    }
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%d:%d", &oh, &om) >= 1) {
            long off = oh * 3600L + om * 60L;
            t -= (*p == '+') ? off : -off;
        }
    }
    return t;
}

/* Reads the string value following `key` (a quoted key plus colon and opening
 * quote, e.g. "\"state\":\"") from `from`, up to `limit`, into out. */
static const char *read_value(const char *from, const char *limit, const char *key, char *out, size_t out_sz)
{
    const char *p = strstr(from, key);
    if (p == NULL || p >= limit) {
        return NULL;
    }
    p += strlen(key);
    size_t n = 0;
    while (p < limit && *p != '"' && n < out_sz - 1) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return p;
}

static void urlencode_iso(time_t t, char *out, size_t sz)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, sz, "%04d-%02d-%02dT%02d:%02d:%02d%%2B00:00", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Line 1: the configured sensor, else whichever temperature sensor the clock
 * is using. Line 2: the configured one, else the BOM observation. */
static const char *graph_entity(const app_settings_t *cfg, int which, char *buf, size_t sz)
{
    if (which == 0) {
        if (cfg->ha_graph_entity[0]) {
            return cfg->ha_graph_entity;
        }
        if (cfg->ha_temp_select == 1 && cfg->ha_temp2_entity[0]) {
            return cfg->ha_temp2_entity;
        }
        return cfg->ha_temp_entity;
    }
    if (cfg->ha_graph2_entity[0]) {
        return cfg->ha_graph2_entity;
    }
    if (cfg->ha_bom_prefix[0] == '\0') {
        return "";
    }
    snprintf(buf, sz, "%stemp", cfg->ha_bom_prefix);
    return buf;
}

/* Base URL with a scheme and no trailing slash; false if it can't be used. */
static bool base_url(const app_settings_t *cfg, char *out, size_t sz)
{
    size_t bl = strlen(cfg->ha_base_url);
    while (bl > 0 && cfg->ha_base_url[bl - 1] == '/') {
        bl--;
    }
    if (strncasecmp(cfg->ha_base_url, "https://", 8) == 0) {
        return false;
    }
    snprintf(out, sz, "%s%.*s", strncasecmp(cfg->ha_base_url, "http://", 7) == 0 ? "" : "http://",
             (int)bl, cfg->ha_base_url);
    return true;
}

/* One sensor's last 24 hours onto the grid ending at `end`. */
static esp_err_t fetch_series(const app_settings_t *cfg, const char *base, const char *entity,
                              time_t end, ha_series_t *out)
{
    time_t start = end - (time_t)(HA_HISTORY_POINTS - 1) * HA_HISTORY_STEP_S - HA_HISTORY_STEP_S;
    char s_iso[64], e_iso[64];
    urlencode_iso(start, s_iso, sizeof(s_iso));
    urlencode_iso(end + HA_HISTORY_STEP_S, e_iso, sizeof(e_iso));

    char *url = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(url, 512, "%s/api/history/period/%s?filter_entity_id=%s&end_time=%s"
             "&minimal_response&no_attributes&significant_changes_only=0",
             base, s_iso, entity, e_iso);
    struct resp_buf resp;
    int status = 0;
    esp_err_t err = http_get(cfg, url, &resp, &status);
    if (err != ESP_OK || status != 200 || resp.data == NULL) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d%s", entity, esp_err_to_name(err), status,
                 status == 401 ? " (token rejected)" : "");
        free(resp.data);
        free(url);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    for (int i = 0; i < HA_HISTORY_POINTS; i++) {
        out->v[i] = NAN;
    }
    strncpy(out->entity, entity, sizeof(out->entity) - 1);

    /* Walk the state changes in order, filling each grid step with the last
     * value at or before it. */
    int next = 0;                  /* next grid step to fill */
    float last = NAN;
    int entries = 0;
    char state[24], when[48];
    const char *p = resp.data, *stop = resp.data + resp.len;
    while (next < HA_HISTORY_POINTS) {
        const char *sp = read_value(p, stop, "\"state\":\"", state, sizeof(state));
        if (sp == NULL) {
            break;
        }
        /* this entry's time sits between its state and the next entry's */
        const char *nx = strstr(sp, "\"state\":\"");
        const char *lim = nx ? nx : stop;
        if (read_value(sp, lim, "\"last_changed\":\"", when, sizeof(when)) == NULL &&
            read_value(sp, lim, "\"last_updated\":\"", when, sizeof(when)) == NULL) {
            p = sp;
            continue;
        }
        p = sp;
        char *endp;
        float val = strtof(state, &endp);
        if (endp == state || *endp != '\0') {
            continue;              /* unavailable / unknown / text */
        }
        time_t at = parse_iso(when);
        if (at < 0) {
            continue;
        }
        entries++;
        /* steps strictly before this change keep the previous value */
        while (next < HA_HISTORY_POINTS) {
            time_t step_t = end - (time_t)(HA_HISTORY_POINTS - 1 - next) * HA_HISTORY_STEP_S;
            if (step_t >= at) {
                break;
            }
            out->v[next++] = last;
        }
        last = val;
    }
    while (next < HA_HISTORY_POINTS) {
        out->v[next++] = last;     /* the final state holds to now */
    }

    out->min = INFINITY;
    out->max = -INFINITY;
    for (int i = 0; i < HA_HISTORY_POINTS; i++) {
        if (isnan(out->v[i])) {
            continue;
        }
        out->have++;
        if (out->v[i] < out->min) {
            out->min = out->v[i];
            out->min_i = i;
        }
        if (out->v[i] > out->max) {
            out->max = out->v[i];
            out->max_i = i;
        }
    }
    ESP_LOGI(TAG, "%s: %d state changes in %u bytes%s -> %d/%d points, %.1f..%.1f", entity, entries,
             (unsigned)resp.len, resp.full ? " (reply cut at the size cap)" : "", out->have,
             HA_HISTORY_POINTS, out->have ? (double)out->min : 0.0, out->have ? (double)out->max : 0.0);
    free(resp.data);

    /* Name and unit, from the entity's current state. */
    snprintf(url, 512, "%s/api/states/%s", base, entity);
    err = http_get(cfg, url, &resp, &status);
    if (err == ESP_OK && status == 200 && resp.data != NULL) {
        cJSON *root = cJSON_Parse(resp.data);
        cJSON *attr = cJSON_GetObjectItemCaseSensitive(root, "attributes");
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(attr, "friendly_name");
        cJSON *un = cJSON_GetObjectItemCaseSensitive(attr, "unit_of_measurement");
        if (cJSON_IsString(fn) && fn->valuestring) {
            strncpy(out->name, fn->valuestring, sizeof(out->name) - 1);
        }
        if (cJSON_IsString(un) && un->valuestring) {
            strncpy(out->unit, un->valuestring, sizeof(out->unit) - 1);
        }
        cJSON_Delete(root);
    }
    free(resp.data);
    free(url);
    return out->have >= 2 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ha_history_refresh(const app_settings_t *cfg)
{
    if (cfg->ha_base_url[0] == '\0' || cfg->ha_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    char names[HA_HISTORY_SERIES][64];
    const char *want[HA_HISTORY_SERIES];
    bool unchanged = true;
    for (int i = 0; i < HA_HISTORY_SERIES; i++) {
        want[i] = graph_entity(cfg, i, names[i], sizeof(names[i]));
        if (strcmp(want[i], s_cur_entity[i]) != 0) {
            unchanged = false;
        }
    }
    if (want[0][0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (unchanged && esp_timer_get_time() < s_next_try_us) {
        return ESP_OK;
    }
    char base[APP_SETTINGS_MAX_STR];
    if (!base_url(cfg, base, sizeof(base))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    time_t end = time(NULL);
    if (end < 1700000000) {
        return ESP_ERR_INVALID_STATE;   /* clock not set */
    }
    end -= end % HA_HISTORY_STEP_S;   /* whole steps, so the time labels land on 15 minutes */

    ha_history_t *fresh = heap_caps_calloc(1, sizeof(*fresh), MALLOC_CAP_SPIRAM);
    if (fresh == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fresh->end = end;
    fresh->step_s = HA_HISTORY_STEP_S;
    esp_err_t first_err = ESP_OK;
    for (int i = 0; i < HA_HISTORY_SERIES; i++) {
        if (want[i][0] == '\0' || (i > 0 && strcmp(want[i], want[0]) == 0)) {
            continue;                    /* nothing to plot, or the same sensor twice */
        }
        esp_err_t err = fetch_series(cfg, base, want[i], end, &fresh->s[fresh->count]);
        if (err == ESP_OK) {
            fresh->count++;
        } else if (first_err == ESP_OK) {
            first_err = err;
        }
    }
    if (fresh->count == 0) {
        free(fresh);
        s_next_try_us = esp_timer_get_time() + RETRY_US;
        return first_err != ESP_OK ? first_err : ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ha_history_t *old = s_cur;
    s_cur = fresh;
    s_ok_us = esp_timer_get_time();
    for (int i = 0; i < HA_HISTORY_SERIES; i++) {
        strncpy(s_cur_entity[i], want[i], sizeof(s_cur_entity[i]) - 1);
    }
    xSemaphoreGive(s_lock);
    free(old);
    /* A line that failed while the other worked is retried sooner. */
    s_next_try_us = esp_timer_get_time() + (first_err == ESP_OK ? REFRESH_US : RETRY_US);
    return ESP_OK;
}

const ha_history_t *ha_history_acquire(void)
{
    if (s_lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cur == NULL || esp_timer_get_time() - s_ok_us > STALE_AFTER_US) {
        return NULL;
    }
    return s_cur;
}

void ha_history_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}
