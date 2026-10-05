#pragma once
/*
 * Current temperature + short weather description, read from Home
 * Assistant's REST API (GET /api/states/<entity_id>) and shown under the
 * ambient clock (see calendar_ui/ui_clock.c).
 *
 * Two HA sensor entities are read, both configured in app_settings_t
 * (ha_temp_entity, ha_desc_entity):
 *   - the temperature sensor's state is a number in degrees C
 *   - the description sensor's state is a short word/phrase such as
 *     "partly_cloudy" - underscores become spaces and the first letter is
 *     capitalised, so the display reads "Partly cloudy" whatever the
 *     integration's exact vocabulary is.
 *
 * HA's built-in sun.sun entity is also read (above_horizon/below_horizon)
 * so the clock can pick a day or night weather icon.
 *
 * Plain http:// on the LAN only (no TLS handshake, so no pressure on the
 * internal-RAM budget calendar sync's TLS already strains). An https://
 * URL is refused with ESP_ERR_NOT_SUPPORTED.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temp_c;
    char desc[32];   /* may be empty if the description sensor is
                        unconfigured, unavailable, or its fetch failed */
    char cond[32];   /* the description sensor's raw state, lower-case, e.g.
                        "mostly_sunny" - for picking an icon; empty with desc */
    int8_t night;    /* from HA's sun.sun: 1 = sun below the horizon,
                        0 = above, -1 = unknown (fetch failed) */
} ha_weather_t;

/* Fetches both entities and updates the cached values. Call from the
 * background network task (blocks for up to a few seconds per request).
 * Returns ESP_ERR_INVALID_STATE if ha_base_url isn't configured, ESP_OK
 * if a fresh temperature was obtained, or another error otherwise (the
 * previously cached values are kept in that case until they go stale).
 * Attempts are throttled: calling again within ~60s of the last attempt
 * returns ESP_OK without touching the network. */
esp_err_t ha_weather_refresh(const app_settings_t *cfg);

/* Copies out the cached values. Returns false when there's nothing
 * trustworthy to show - never fetched successfully yet, or the last
 * success is more than ~45 minutes old - so the UI shows nothing rather
 * than an old temperature presented as current. Cheap and safe to call
 * from any task, including the LVGL task. */
bool ha_weather_get(ha_weather_t *out);

/* ----- Weather details panel (tap the temperature on the clock) -----
 * Fetched in the same refresh as the temperature, in one POST to Home
 * Assistant's /api/template that returns every value (with its unit) as
 * JSON - see ha_weather_details.c. Strings are UTF-8 and empty when the
 * entity isn't configured or is unknown/unavailable. */
typedef struct {
    char value[24];
    char unit[12];
} ha_reading_t;

#define HA_FORECAST_DAYS 7

typedef struct {
    char short_text[72];
    char icon[24];          /* BOM icon_descriptor, e.g. "mostly_sunny" */
    char lo[8], hi[8];      /* temp_min / temp_max, in degrees C */
    char rain_chance[8];    /* percent */
    char rain_range[16];    /* mm, e.g. "3–10" */
    char extended[420];     /* long forecast - days 0 and 1 only */
} ha_forecast_day_t;

typedef struct {
    ha_reading_t feels, wind, gust, wdir, rain, pressure;
    ha_forecast_day_t days[HA_FORECAST_DAYS];
} ha_weather_details_t;

/* The latest details, or NULL if there are none or they're over ~45 minutes
 * old. Holds a lock until ha_weather_details_release() - keep it brief, and
 * always release, NULL or not. */
const ha_weather_details_t *ha_weather_details_acquire(void);
void ha_weather_details_release(void);

/* Internal: called from ha_weather_refresh(). */
esp_err_t ha_weather_details_refresh(const app_settings_t *cfg);

#ifdef __cplusplus
}
#endif
