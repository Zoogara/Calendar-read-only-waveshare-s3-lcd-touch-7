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

#ifdef __cplusplus
}
#endif
