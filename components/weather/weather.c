#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "weather";

#define REFRESH_US       (15LL * 60 * 1000000)
#define RETRY_US         (5LL * 60 * 1000000)
#define STALE_US         (90LL * 60 * 1000000)
#define HTTP_TIMEOUT_MS  10000

/* Plain http: Open-Meteo serves it, and it keeps a TLS handshake off the
 * internal-RAM budget. Nothing private is sent - just a location. */
#define URL_FMT \
    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m," \
    "wind_direction_10m,wind_gusts_10m,pressure_msl" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum," \
    "precipitation_probability_max,wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant" \
    "&hourly=precipitation&past_hours=%d&forecast_hours=0" \
    "&timezone=auto&forecast_days=7"

typedef struct {
    weather_now_t now;
    weather_details_t details;
} snapshot_t;

static SemaphoreHandle_t s_lock;
static snapshot_t *s_snap;            /* PSRAM; NULL until the first success */
static int64_t s_ok_us = -1;
static int64_t s_attempt_us = -1;
static bool s_last_failed;

/* ---------- WMO weather codes -> icon condition + wording ---------- */

typedef struct {
    int code;
    const char *cond;   /* matches ui_weather_glyph()'s table (BOM-style names) */
    const char *desc;
} wmo_t;

static const wmo_t WMO[] = {
    { 0,  "sunny",          "Clear" },
    { 1,  "mostly_sunny",   "Mainly clear" },
    { 2,  "partly_cloudy",  "Partly cloudy" },
    { 3,  "cloudy",         "Overcast" },
    { 45, "fog",            "Fog" },
    { 48, "fog",            "Freezing fog" },
    { 51, "light_rain",     "Light drizzle" },
    { 53, "light_rain",     "Drizzle" },
    { 55, "light_rain",     "Heavy drizzle" },
    { 56, "light_rain",     "Freezing drizzle" },
    { 57, "light_rain",     "Freezing drizzle" },
    { 61, "light_rain",     "Slight rain" },
    { 63, "rain",           "Rain" },
    { 65, "rain",           "Heavy rain" },
    { 66, "rain",           "Freezing rain" },
    { 67, "rain",           "Freezing rain" },
    { 71, "snow",           "Slight snow" },
    { 73, "snow",           "Snow" },
    { 75, "snow",           "Heavy snow" },
    { 77, "snow",           "Snow grains" },
    { 80, "shower",         "Slight showers" },
    { 81, "showers",        "Showers" },
    { 82, "heavy_showers",  "Heavy showers" },
    { 85, "snow",           "Snow showers" },
    { 86, "snow",           "Heavy snow showers" },
    { 95, "storm",          "Thunderstorm" },
    { 96, "storm",          "Thunderstorm, hail" },
    { 99, "storm",          "Thunderstorm, hail" },
};

static const wmo_t *wmo(int code)
{
    for (size_t i = 0; i < sizeof(WMO) / sizeof(WMO[0]); i++) {
        if (WMO[i].code == code) {
            return &WMO[i];
        }
    }
    return NULL;
}

static const char *compass(double deg)
{
    static const char *POINTS[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    int i = (int)floor(fmod(deg + 11.25 + 360.0, 360.0) / 22.5);
    return POINTS[i & 15];
}

/* ---------- parsing helpers ---------- */

static bool num(cJSON *obj, const char *key, double *out)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(j)) {
        return false;
    }
    *out = j->valuedouble;
    return true;
}

static bool num_at(cJSON *obj, const char *key, int i, double *out)
{
    cJSON *j = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, key), i);
    if (!cJSON_IsNumber(j)) {
        return false;
    }
    *out = j->valuedouble;
    return true;
}

static void unit_of(cJSON *units, const char *key, char *out, size_t sz)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(units, key);
    snprintf(out, sz, "%s", cJSON_IsString(j) ? j->valuestring : "");
}

/* A current reading with its unit, or left empty if Open-Meteo didn't
 * provide it. */
static void reading(weather_reading_t *r, cJSON *cur, cJSON *units, const char *key, const char *fmt)
{
    double v;
    if (num(cur, key, &v)) {
        snprintf(r->value, sizeof(r->value), fmt, v);
        unit_of(units, key, r->unit, sizeof(r->unit));
    }
}

static void parse(cJSON *root, snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "current_units");
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    weather_details_t *d = &out->details;
    double v;

    /* Now */
    if (num(cur, "temperature_2m", &v)) {
        out->now.temp_c = (float)v;
    }
    if (num(cur, "is_day", &v)) {
        out->now.night = (v == 0);
    }
    if (num(cur, "weather_code", &v)) {
        const wmo_t *w = wmo((int)v);
        if (w) {
            snprintf(out->now.cond, sizeof(out->now.cond), "%s", w->cond);
            snprintf(out->now.desc, sizeof(out->now.desc), "%s", w->desc);
        }
    }
    reading(&d->temp, cur, cu, "temperature_2m", "%.1f");
    reading(&d->feels, cur, cu, "apparent_temperature", "%.1f");
    reading(&d->wind, cur, cu, "wind_speed_10m", "%.0f");
    reading(&d->gust, cur, cu, "wind_gusts_10m", "%.0f");
    reading(&d->pressure, cur, cu, "pressure_msl", "%.0f");
    if (num(cur, "wind_direction_10m", &v)) {
        snprintf(d->wdir.value, sizeof(d->wdir.value), "%s", compass(v));
    }

    /* Rain since local midnight: the hourly amounts before the current hour
     * (the request asks for exactly those - see weather_refresh()). */
    cJSON *precip = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation");
    if (cJSON_IsArray(precip)) {
        double sum = 0;
        cJSON *p;
        cJSON_ArrayForEach(p, precip) {
            if (cJSON_IsNumber(p)) {
                sum += p->valuedouble;
            }
        }
        snprintf(d->rain.value, sizeof(d->rain.value), "%.1f", sum);
        snprintf(d->rain.unit, sizeof(d->rain.unit), "mm");
    }

    /* 7-day forecast */
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        weather_day_t *f = &d->days[i];
        if (num_at(daily, "weather_code", i, &v)) {
            const wmo_t *w = wmo((int)v);
            if (w) {
                snprintf(f->icon, sizeof(f->icon), "%s", w->cond);
                snprintf(f->short_text, sizeof(f->short_text), "%s", w->desc);
            }
        }
        if (num_at(daily, "temperature_2m_min", i, &v)) {
            snprintf(f->lo, sizeof(f->lo), "%.0f", v);
        }
        if (num_at(daily, "temperature_2m_max", i, &v)) {
            snprintf(f->hi, sizeof(f->hi), "%.0f", v);
        }
        double chance = -1, amount = -1;
        if (num_at(daily, "precipitation_probability_max", i, &chance)) {
            snprintf(f->rain_chance, sizeof(f->rain_chance), "%.0f", chance);
        }
        if (num_at(daily, "precipitation_sum", i, &amount)) {
            snprintf(f->rain_amount, sizeof(f->rain_amount), "%.1f", amount);
        }

        /* No written forecast from Open-Meteo - compose one for Today and
         * Tomorrow from the numbers. */
        if (i < 2) {
            int n = snprintf(f->extended, sizeof(f->extended), "%s.", f->short_text[0] ? f->short_text : "");
            if (chance > 0 || amount > 0.05) {
                n += snprintf(f->extended + n, sizeof(f->extended) - n, " %.0f%% chance of rain, about %.1f mm.",
                              chance > 0 ? chance : 0, amount > 0 ? amount : 0);
            } else if (chance == 0) {
                n += snprintf(f->extended + n, sizeof(f->extended) - n, " No rain expected.");
            }
            double wmax, gmax, wdir;
            if (num_at(daily, "wind_speed_10m_max", i, &wmax) && (size_t)n < sizeof(f->extended)) {
                bool has_dir = num_at(daily, "wind_direction_10m_dominant", i, &wdir);
                n += snprintf(f->extended + n, sizeof(f->extended) - n, " Winds %s%sup to %.0f km/h",
                              has_dir ? compass(wdir) : "", has_dir ? " " : "", wmax);
                if (num_at(daily, "wind_gusts_10m_max", i, &gmax) && (size_t)n < sizeof(f->extended)) {
                    n += snprintf(f->extended + n, sizeof(f->extended) - n, ", gusts to %.0f", gmax);
                }
                if ((size_t)n < sizeof(f->extended)) {
                    snprintf(f->extended + n, sizeof(f->extended) - n, ".");
                }
            }
        }
    }
}

/* ---------- fetching ---------- */

struct resp_buf {
    char *data;
    size_t len, cap;
};

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    struct resp_buf *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + evt->data_len + 1 > b->cap) {
            size_t cap = (b->len + evt->data_len + 1) * 2;
            char *g = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM);
            if (g == NULL) {
                return ESP_FAIL;
            }
            b->data = g;
            b->cap = cap;
        }
        memcpy(b->data + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->data[b->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch(const app_settings_t *cfg, int past_hours, snapshot_t *out)
{
    char url[640];
    int n = snprintf(url, sizeof(url), URL_FMT, cfg->weather_lat, cfg->weather_lon, past_hours);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct resp_buf resp = { .data = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM), .cap = 4096 };
    if (resp.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';
    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_data,
        .user_data = &resp,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,   /* the request line is ~520 bytes */
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        free(resp.data);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        /* Open-Meteo explains a 400 in the body, e.g. a bad latitude. */
        ESP_LOGW(TAG, "request failed: %s, HTTP %d %.160s", esp_err_to_name(err), status, resp.data);
        free(resp.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    size_t bytes = resp.len;
    cJSON *root = cJSON_Parse(resp.data);   /* PSRAM - see main.c's cJSON hooks */
    free(resp.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad JSON (%u bytes)", (unsigned)bytes);
        return ESP_ERR_INVALID_RESPONSE;
    }
    parse(root, out);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u bytes: %.1f C %s%s, wind %s %s %s, rain today %s mm, day 0 %s/%s %s",
             (unsigned)bytes, out->now.temp_c, out->now.desc, out->now.night ? " (night)" : "",
             out->details.wdir.value, out->details.wind.value, out->details.wind.unit,
             out->details.rain.value, out->details.days[0].lo, out->details.days[0].hi,
             out->details.days[0].icon);
    return ESP_OK;
}

esp_err_t weather_refresh(const app_settings_t *cfg)
{
    if (cfg->weather_lat[0] == '\0' || cfg->weather_lon[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year < 120) {
        return ESP_ERR_INVALID_STATE;   /* clock not set yet */
    }
    int64_t t = esp_timer_get_time();
    if (s_attempt_us >= 0 && t - s_attempt_us < (s_last_failed ? RETRY_US : REFRESH_US)) {
        return ESP_OK;
    }
    s_attempt_us = t;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    snapshot_t *fresh = heap_caps_malloc(sizeof(*fresh), MALLOC_CAP_SPIRAM);
    if (fresh == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* past_hours = whole hours since local midnight, for "rain today". */
    esp_err_t err = fetch(cfg, lt.tm_hour, fresh);
    s_last_failed = (err != ESP_OK);
    if (err != ESP_OK) {
        free(fresh);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot_t *old = s_snap;
    s_snap = fresh;
    s_ok_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    free(old);   /* readers hold the lock while using it */
    return ESP_OK;
}

bool weather_get(weather_now_t *out)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_snap != NULL && esp_timer_get_time() - s_ok_us < STALE_US;
    if (ok) {
        *out = s_snap->now;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

const weather_details_t *weather_details_acquire(void)
{
    if (s_lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap == NULL || esp_timer_get_time() - s_ok_us >= STALE_US) {
        return NULL;
    }
    return &s_snap->details;
}

void weather_details_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}
