#pragma once
/*
 * Weather for the ambient clock and its details panel, from Open-Meteo
 * (https://open-meteo.com - free, no API key, for non-commercial use) for
 * the latitude/longitude set on the config page. One plain-http request
 * (~1.5KB of JSON) brings everything: current conditions, rain since local
 * midnight, and a 7-day daily forecast. Refreshed every 15 minutes from the
 * background network task; held in PSRAM.
 *
 * Open-Meteo is model data, not a station: "rain today" is the model's
 * estimate, and there's no written forecast - Today and Tomorrow get a
 * summary composed from the numbers instead.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For the clock: temperature and the icon to show. */
typedef struct {
    float temp_c;
    char cond[24];     /* icon condition, e.g. "partly_cloudy" (see ui_weather_glyph()) */
    char desc[40];     /* e.g. "Partly cloudy" */
    bool night;        /* Open-Meteo's is_day == 0 */
} weather_now_t;

typedef struct {
    char value[16];
    char unit[12];
} weather_reading_t;

#define WEATHER_FORECAST_DAYS 7

typedef struct {
    char short_text[40];   /* e.g. "Slight rain" */
    char icon[24];         /* icon condition, as weather_now_t.cond */
    char lo[8], hi[8];     /* whole degrees C */
    char rain_chance[8];   /* percent */
    char rain_amount[12];  /* mm, e.g. "3.0" */
    char extended[200];    /* composed summary - days 0 and 1 only */
} weather_day_t;

typedef struct {
    weather_reading_t temp, feels, wind, gust, wdir, rain, pressure;
    weather_day_t days[WEATHER_FORECAST_DAYS];
} weather_details_t;

/* Fetches if due (every 15 minutes, failures retried after 5). Call from
 * the background network task; needs the clock set. No-op without a
 * configured latitude. */
esp_err_t weather_refresh(const app_settings_t *cfg);

/* False when there's nothing current enough to show (never fetched, or the
 * last success is over 90 minutes old). Cheap; any task. */
bool weather_get(weather_now_t *out);

/* The details, or NULL (same staleness rule). Holds a lock until
 * weather_details_release() - keep it brief and always release, and don't
 * call weather_get() in between: it takes the same (non-recursive) lock,
 * so the calling task would deadlock. The details carry everything the
 * panel needs, including the current temperature. */
const weather_details_t *weather_details_acquire(void);
void weather_details_release(void);

/* ----- Graphs and the hourly forecast (weather panel) -----
 * One more Open-Meteo request pair (see weather_series.c) feeds these: an
 * hourly one for the last 24 hours and the next 24, and a daily one for the
 * last 14 days. The hourly values are interpolated onto 15-minute steps for the
 * two line graphs. Model data, like everything here - not a station. */

/* Last 24 hours of temperature: WEATHER_HISTORY_POINTS steps of
 * WEATHER_HISTORY_STEP_S ending at `end`. */
#define WEATHER_HISTORY_POINTS  96
#define WEATHER_HISTORY_STEP_S  900
#define WEATHER_HISTORY_SERIES  1
typedef struct {
    float v[WEATHER_HISTORY_POINTS];
    int slot;                     /* graph line colour: 0 */
    int have;
    float min, max;
    int min_i, max_i;             /* grid positions of min and max */
    char name[48];
    char unit[12];
} weather_series_t;
typedef struct {
    time_t end;
    int step_s;
    int count;
    weather_series_t s[WEATHER_HISTORY_SERIES];
} weather_history_t;

/* Daily low / high and rainfall, today and the 13 days before. */
#define WEATHER_WEEK_DAYS 14
typedef struct {
    time_t day[WEATHER_WEEK_DAYS];   /* local midnight of each day, oldest first, today last */
    float tmin[WEATHER_WEEK_DAYS], tmax[WEATHER_WEEK_DAYS];
    bool t_ok[WEATHER_WEEK_DAYS];
    int t_have;
    float rain[WEATHER_WEEK_DAYS];
    bool r_day_ok[WEATHER_WEEK_DAYS];
    bool r_ok;
    char t_unit[12], r_unit[12];
} weather_week_t;

/* Wind over the last 24 hours: speed and gust lines, an arrow per hour. */
#define WEATHER_WIND_STEPS 96
#define WEATHER_WIND_HOURS 24
typedef struct {
    time_t end;
    float speed[WEATHER_WIND_STEPS];
    float gust[WEATHER_WIND_STEPS];
    bool gust_ok, dir_ok;
    float hr_dir[WEATHER_WIND_HOURS];     /* degrees the wind comes FROM */
    float hr_speed[WEATHER_WIND_HOURS];
    bool hr_calm[WEATHER_WIND_HOURS];
    float now_speed, now_dir;             /* now_dir -1 = calm */
    float prevailing;                     /* -1 = none */
    float max_gust;
    int max_gust_i;
    char unit[12];
} weather_wind_t;

/* The next 24 hours, hour 0 being the current one. */
#define WEATHER_HOURLY_MAX 24
typedef struct {
    int count;
    time_t t[WEATHER_HOURLY_MAX];
    float temp[WEATHER_HOURLY_MAX];
    char cond[WEATHER_HOURLY_MAX][20];    /* icon condition, as weather_now_t.cond */
    float rain_mm[WEATHER_HOURLY_MAX];
    int rain_pct[WEATHER_HOURLY_MAX];     /* -1 if not given */
    bool night[WEATHER_HOURLY_MAX];
} weather_hourly_t;

/* Internal, shared with weather.c. */
const char *weather_wmo_icon(int wmo_code);   /* icon condition ("" if unknown) */

/* Fetched from the background network task (hourly data every 15 minutes, the
 * 14-day data hourly; failures retried after 5). No-op without a latitude. */
esp_err_t weather_series_refresh(const app_settings_t *cfg);

/* Each returns the data, or NULL if there is none or it is over 2 hours old.
 * Holds a lock until the matching release - keep it brief, always release. */
const weather_history_t *weather_history_acquire(void);
void weather_history_release(void);
const weather_wind_t *weather_wind_acquire(void);
void weather_wind_release(void);
const weather_hourly_t *weather_hourly_acquire(void);
void weather_hourly_release(void);
const weather_week_t *weather_week_acquire(void);
void weather_week_release(void);

/* True while the last attempt at any of the above failed. */
bool weather_graphs_incomplete(void);

#ifdef __cplusplus
}
#endif
