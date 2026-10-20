#include "weather.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "weather_series";

#define HOURLY_EVERY_US  (15LL * 60 * 1000000)
#define WEEK_EVERY_US    (60LL * 60 * 1000000)
#define RETRY_US         (5LL * 60 * 1000000)
#define STALE_US         (120LL * 60 * 1000000)
#define CALM_BELOW       2.0f      /* km/h: no meaningful direction under this */

/* 24 hours back and 24 forward in one go: index 24 is the current hour. */
#define HOURLY_URL \
    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
    "&hourly=temperature_2m,weather_code,is_day,precipitation,precipitation_probability," \
    "wind_speed_10m,wind_gusts_10m,wind_direction_10m" \
    "&past_hours=24&forecast_hours=24&timeformat=unixtime&timezone=auto"
#define DAILY_URL \
    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum" \
    "&past_days=13&forecast_days=1&timeformat=unixtime&timezone=auto"

cJSON *weather_get_json(const char *url);   /* weather.c */

typedef struct {
    weather_history_t history;
    weather_wind_t wind;
    weather_hourly_t hourly;
    weather_week_t week;
} snap_t;

static SemaphoreHandle_t s_lock;
static snap_t *s_snap;                      /* PSRAM */
static int64_t s_hourly_ok_us = -1, s_week_ok_us = -1;
static int64_t s_hourly_try_us = -1, s_week_try_us = -1;
static bool s_hourly_failed, s_week_failed;

/* ---------- helpers ---------- */

static float at(cJSON *arr, int i)
{
    cJSON *j = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(j) ? (float)j->valuedouble : NAN;
}

/* A value `hours_back` hours (fractional) before hourly index i0, interpolated. */
static float lerp_back(const float *v, int n, int i0, float hours_back)
{
    int a = (int)floorf(hours_back);
    float f = hours_back - (float)a;
    int ia = i0 - a, ib = ia - 1;
    if (ia < 0 || ia >= n || isnan(v[ia])) {
        return NAN;
    }
    if (f < 0.001f || ib < 0 || isnan(v[ib])) {
        return v[ia];
    }
    return v[ia] * (1.0f - f) + v[ib] * f;
}

/* ---------- hourly: history, wind and the forecast strip ---------- */

#define HN 48
static void build_hourly(cJSON *hourly, snap_t *out)
{
    cJSON *tarr = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    int n = cJSON_GetArraySize(tarr);
    if (n > HN) {
        n = HN;
    }
    float temp[HN], speed[HN], gust[HN], dir[HN], rain[HN], pct[HN], code[HN], isday[HN];
    time_t tm_[HN];
    cJSON *jt = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *js = cJSON_GetObjectItemCaseSensitive(hourly, "wind_speed_10m");
    cJSON *jg = cJSON_GetObjectItemCaseSensitive(hourly, "wind_gusts_10m");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(hourly, "wind_direction_10m");
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
    cJSON *jc = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    cJSON *ji = cJSON_GetObjectItemCaseSensitive(hourly, "is_day");
    for (int i = 0; i < n; i++) {
        tm_[i] = (time_t)at(tarr, i);
        temp[i] = at(jt, i);
        speed[i] = at(js, i);
        gust[i] = at(jg, i);
        dir[i] = at(jd, i);
        rain[i] = at(jr, i);
        pct[i] = at(jp, i);
        code[i] = at(jc, i);
        isday[i] = at(ji, i);
    }

    /* The current hour: the last one that has started. */
    time_t now = time(NULL);
    int i0 = -1;
    for (int i = 0; i < n; i++) {
        if (tm_[i] <= now) {
            i0 = i;
        }
    }
    if (i0 < 0) {
        return;
    }

    /* Temperature history on 15-minute steps, ending at the current hour. */
    weather_history_t *h = &out->history;
    memset(h, 0, sizeof(*h));
    h->end = tm_[i0];
    h->step_s = WEATHER_HISTORY_STEP_S;
    weather_series_t *s = &h->s[0];
    snprintf(s->name, sizeof(s->name), "Temperature");
    snprintf(s->unit, sizeof(s->unit), "\xC2\xB0" "C");
    s->min = INFINITY;
    s->max = -INFINITY;
    for (int j = 0; j < WEATHER_HISTORY_POINTS; j++) {
        float back = (float)(WEATHER_HISTORY_POINTS - 1 - j) * 0.25f;
        s->v[j] = lerp_back(temp, n, i0, back);
        if (!isnan(s->v[j])) {
            s->have++;
            if (s->v[j] < s->min) {
                s->min = s->v[j];
                s->min_i = j;
            }
            if (s->v[j] > s->max) {
                s->max = s->v[j];
                s->max_i = j;
            }
        }
    }
    h->count = s->have > 1 ? 1 : 0;

    /* Wind: lines on the same steps; an arrow per hour. */
    weather_wind_t *w = &out->wind;
    memset(w, 0, sizeof(*w));
    w->end = tm_[i0];
    snprintf(w->unit, sizeof(w->unit), "km/h");
    w->now_dir = -1;
    w->trend = -1;
    w->max_gust_i = -1;
    float sx = 0, sy = 0;
    for (int j = 0; j < WEATHER_WIND_STEPS; j++) {
        float back = (float)(WEATHER_WIND_STEPS - 1 - j) * 0.25f;
        w->speed[j] = lerp_back(speed, n, i0, back);
        w->gust[j] = lerp_back(gust, n, i0, back);
        if (!isnan(w->gust[j])) {
            w->gust_ok = true;
            if (w->max_gust_i < 0 || w->gust[j] > w->max_gust) {
                w->max_gust = w->gust[j];
                w->max_gust_i = j;
            }
        }
    }
    for (int k = 0; k < WEATHER_WIND_HOURS; k++) {
        int idx = i0 - (WEATHER_WIND_HOURS - 1 - k);
        bool ok = idx >= 0 && !isnan(speed[idx]) && !isnan(dir[idx]);
        w->hr_speed[k] = idx >= 0 ? speed[idx] : NAN;
        w->hr_dir[k] = ok ? dir[idx] : NAN;
        w->hr_calm[k] = !ok || speed[idx] < CALM_BELOW;
        if (!w->hr_calm[k]) {
            w->dir_ok = true;
            if (k >= WEATHER_WIND_HOURS - WEATHER_WIND_TREND_HOURS) {
                float rad = dir[idx] * (float)M_PI / 180.0f;
                sx += speed[idx] * sinf(rad);
                sy += speed[idx] * cosf(rad);
            }
        }
    }
    w->now_speed = speed[i0];
    w->now_dir = w->hr_calm[WEATHER_WIND_HOURS - 1] ? -1 : w->hr_dir[WEATHER_WIND_HOURS - 1];
    if (sx != 0 || sy != 0) {
        w->trend = fmodf(atan2f(sx, sy) * 180.0f / (float)M_PI + 360.0f, 360.0f);
    }

    /* The forecast strip: this hour and the 23 after. */
    weather_hourly_t *f = &out->hourly;
    memset(f, 0, sizeof(*f));
    for (int i = i0; i < n && f->count < WEATHER_HOURLY_MAX; i++) {
        int k = f->count++;
        f->t[k] = tm_[i];
        f->temp[k] = temp[i];
        snprintf(f->cond[k], sizeof(f->cond[k]), "%s", isnan(code[i]) ? "" : weather_wmo_icon((int)code[i]));
        f->rain_mm[k] = isnan(rain[i]) ? 0 : rain[i];
        f->rain_pct[k] = isnan(pct[i]) ? -1 : (int)lroundf(pct[i]);
        f->night[k] = !isnan(isday[i]) && isday[i] == 0;
    }
}

/* ---------- daily: the 14-day bars ---------- */

static void build_week(cJSON *daily, weather_week_t *wk)
{
    memset(wk, 0, sizeof(*wk));
    cJSON *jmax = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *jmin = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_sum");
    int n = cJSON_GetArraySize(jmax);
    if (n > WEATHER_WEEK_DAYS) {
        n = WEATHER_WEEK_DAYS;
    }
    snprintf(wk->t_unit, sizeof(wk->t_unit), "\xC2\xB0" "C");
    snprintf(wk->r_unit, sizeof(wk->r_unit), "mm");

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    for (int d = 0; d < WEATHER_WEEK_DAYS; d++) {
        /* Reply index of this day: the reply ends with today. */
        int ri = n - WEATHER_WEEK_DAYS + d;
        struct tm dt = lt;
        dt.tm_mday -= (WEATHER_WEEK_DAYS - 1 - d);
        dt.tm_isdst = -1;
        wk->day[d] = mktime(&dt);
        if (ri < 0) {
            continue;
        }
        float lo = at(jmin, ri), hi = at(jmax, ri), r = at(jr, ri);
        wk->tmin[d] = lo;
        wk->tmax[d] = hi;
        wk->t_ok[d] = !isnan(lo) && !isnan(hi);
        if (wk->t_ok[d]) {
            wk->t_have++;
        }
        wk->rain[d] = isnan(r) ? 0 : r;
        wk->r_day_ok[d] = !isnan(r);
        wk->r_ok = wk->r_ok || wk->r_day_ok[d];
    }
}

/* ---------- refresh ---------- */

static esp_err_t publish(const snap_t *fresh_parts, bool hourly, bool week)
{
    snap_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snap_t *old = s_snap;
    if (old != NULL) {
        *s = *old;
    }
    if (hourly) {
        s->history = fresh_parts->history;
        s->wind = fresh_parts->wind;
        s->hourly = fresh_parts->hourly;
    }
    if (week) {
        s->week = fresh_parts->week;
    }
    s_snap = s;
    xSemaphoreGive(s_lock);
    free(old);
    return ESP_OK;
}

esp_err_t weather_series_refresh(const app_settings_t *cfg)
{
    if (cfg->weather_lat[0] == '\0' || cfg->weather_lon[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year < 120) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    int64_t t = esp_timer_get_time();
    bool do_hourly = s_hourly_try_us < 0 || t - s_hourly_try_us >= (s_hourly_failed ? RETRY_US : HOURLY_EVERY_US);
    bool do_week = s_week_try_us < 0 || t - s_week_try_us >= (s_week_failed ? RETRY_US : WEEK_EVERY_US);
    if (!do_hourly && !do_week) {
        return ESP_OK;
    }
    snap_t *parts = heap_caps_calloc(1, sizeof(*parts), MALLOC_CAP_SPIRAM);
    if (parts == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char url[512];
    esp_err_t result = ESP_OK;

    if (do_hourly) {
        s_hourly_try_us = t;
        snprintf(url, sizeof(url), HOURLY_URL, cfg->weather_lat, cfg->weather_lon);
        cJSON *root = weather_get_json(url);
        cJSON *hourly = root ? cJSON_GetObjectItemCaseSensitive(root, "hourly") : NULL;
        if (hourly != NULL) {
            build_hourly(hourly, parts);
        }
        s_hourly_failed = (hourly == NULL || parts->hourly.count == 0);
        if (!s_hourly_failed) {
            publish(parts, true, false);
            s_hourly_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "hourly: %d forecast hours, %.1f..%.1f C over 24 h, gust max %.0f",
                     parts->hourly.count, (double)parts->history.s[0].min, (double)parts->history.s[0].max,
                     (double)parts->wind.max_gust);
        } else {
            result = ESP_FAIL;
        }
        if (root != NULL) {
            cJSON_Delete(root);
        }
    }
    if (do_week) {
        s_week_try_us = esp_timer_get_time();
        snprintf(url, sizeof(url), DAILY_URL, cfg->weather_lat, cfg->weather_lon);
        cJSON *root = weather_get_json(url);
        cJSON *daily = root ? cJSON_GetObjectItemCaseSensitive(root, "daily") : NULL;
        if (daily != NULL) {
            build_week(daily, &parts->week);
        }
        s_week_failed = (daily == NULL || parts->week.t_have == 0);
        if (!s_week_failed) {
            publish(parts, false, true);
            s_week_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "daily: %d days with temperatures", parts->week.t_have);
        } else {
            result = ESP_FAIL;
        }
        if (root != NULL) {
            cJSON_Delete(root);
        }
    }
    free(parts);
    return result;
}

/* ---------- readers ---------- */

static const void *acquire(size_t offset, int64_t ok_us)
{
    if (s_lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap == NULL || ok_us < 0 || esp_timer_get_time() - ok_us >= STALE_US) {
        return NULL;
    }
    return (const char *)s_snap + offset;
}

static void release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

const weather_history_t *weather_history_acquire(void)
{
    return acquire(offsetof(snap_t, history), s_hourly_ok_us);
}
void weather_history_release(void) { release(); }
const weather_wind_t *weather_wind_acquire(void) { return acquire(offsetof(snap_t, wind), s_hourly_ok_us); }
void weather_wind_release(void) { release(); }
const weather_hourly_t *weather_hourly_acquire(void) { return acquire(offsetof(snap_t, hourly), s_hourly_ok_us); }
void weather_hourly_release(void) { release(); }
const weather_week_t *weather_week_acquire(void) { return acquire(offsetof(snap_t, week), s_week_ok_us); }
void weather_week_release(void) { release(); }

bool weather_graphs_incomplete(void)
{
    return s_hourly_failed || s_week_failed;
}
