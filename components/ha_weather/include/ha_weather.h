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
#include <time.h>
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
    char extended[800];     /* long forecast - days 0 and 1 only; from the
                               sensors' "state" attribute, which isn't cut
                               at 255 characters like the state itself */
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

/* ----- Sensor history graph (bottom of the weather details panel) -----
 * The last 24 hours of up to two sensors, each resampled onto
 * HA_HISTORY_POINTS steps of HA_HISTORY_STEP_S ending at `end`: series 0 is
 * ha_graph_entity (else the temperature sensor the clock shows), series 1 is
 * ha_graph2_entity (else the BOM observation "<ha_bom_prefix>temp"). See
 * ha_history.c. */
#define HA_HISTORY_POINTS  96
#define HA_HISTORY_STEP_S  900
#define HA_HISTORY_SERIES  2

typedef struct {
    float v[HA_HISTORY_POINTS];   /* NAN where the sensor had no value yet */
    int slot;                     /* 0 = graph line 1, 1 = line 2 (colour and label) */
    int have;                     /* how many points have a value */
    float min, max;
    int min_i, max_i;             /* grid positions of min and max */
    char entity[64];
    char name[48];                /* friendly name, may be empty */
    char unit[12];                /* e.g. "°C", may be empty */
} ha_series_t;

typedef struct {
    time_t end;                   /* time of v[HA_HISTORY_POINTS - 1] */
    int step_s;
    int count;                    /* series with data: they are s[0..count-1] */
    ha_series_t s[HA_HISTORY_SERIES];
} ha_history_t;

/* Fetched at most every ~10 minutes (a failure is retried after 3); call from
 * the background network task. Done as part of ha_weather_refresh(). */
esp_err_t ha_history_refresh(const app_settings_t *cfg);

/* The latest history, or NULL if there is none or it is over an hour old.
 * Holds a lock until ha_history_release() - keep it brief, always release. */
const ha_history_t *ha_history_acquire(void);
void ha_history_release(void);

/* ----- 14-day bar graphs (below the 24-hour graph) -----
 * Daily low / high of the temperature sensor (graph line 1) and daily rainfall
 * (ha_rain_entity, a running total that resets - summed from its rises), for
 * today and the six days before, bucketed by the display's local midnight.
 * Fetched every ~30 minutes. See ha_history.c. */
#define HA_WEEK_DAYS 14

typedef struct {
    time_t day[HA_WEEK_DAYS];       /* local midnight of each day, oldest first, today last */
    float tmin[HA_WEEK_DAYS], tmax[HA_WEEK_DAYS];
    bool t_ok[HA_WEEK_DAYS];        /* the sensor had a value that day */
    int t_have;
    float rain[HA_WEEK_DAYS];
    bool r_day_ok[HA_WEEK_DAYS];    /* rain history covers that day (HA keeps ~10 days by default) */
    bool r_ok;                      /* rain data present at all */
    char t_unit[12], r_unit[12];
} ha_week_t;

esp_err_t ha_week_refresh(const app_settings_t *cfg);
const ha_week_t *ha_week_acquire(void);   /* NULL if none / over 2 hours old */
void ha_week_release(void);

/* ----- Wind graph (24 hours): speed and gust lines with hourly direction arrows -----
 * From the speed (ha_wind_entity), gust (ha_gust_entity) and direction in
 * degrees (ha_wdeg_entity) histories. Speed is the time-weighted mean of each
 * 15-minute step and gust the highest in it; each hour's arrow is the
 * speed-weighted vector mean direction, calm hours having none. See
 * ha_history.c and ha_wind_calc.c. Refreshed every ~10 minutes. */
#define HA_WIND_STEPS 96
#define HA_WIND_HOURS 24

typedef struct {
    time_t end;                       /* end of the last 15-minute step */
    float speed[HA_WIND_STEPS];       /* NAN where the sensor had no value */
    float gust[HA_WIND_STEPS];
    bool gust_ok, dir_ok;
    float hr_dir[HA_WIND_HOURS];      /* degrees the wind comes FROM */
    float hr_speed[HA_WIND_HOURS];
    bool hr_calm[HA_WIND_HOURS];      /* no meaningful direction that hour */
    float now_speed, now_dir;         /* the latest speed; direction of the latest hour (-1 = calm) */
    float prevailing;                 /* over the 24 hours (-1 = none) */
    float max_gust;
    int max_gust_i;                   /* step of the highest gust */
    char unit[12];
} ha_wind_t;

esp_err_t ha_wind_refresh(const app_settings_t *cfg);
const ha_wind_t *ha_wind_acquire(void);   /* NULL if none / over an hour old */
void ha_wind_release(void);

/* ----- Hourly forecast strip (weather panel) -----
 * The next 24 hours from the hourly forecast sensor (ha_hourly_entity): one
 * small GET of its state attributes, refreshed every ~5 minutes. Hour 0 is
 * the current hour. See ha_history.c. */
#define HA_HOURLY_MAX 24

typedef struct {
    int count;
    time_t t[HA_HOURLY_MAX];
    float temp[HA_HOURLY_MAX];
    char cond[HA_HOURLY_MAX][20];     /* Home Assistant condition: "sunny", "clear-night", "rainy" ... */
    float rain_mm[HA_HOURLY_MAX];
    int rain_pct[HA_HOURLY_MAX];      /* chance of rain, percent; -1 if not given */
    bool night[HA_HOURLY_MAX];        /* after sunset / before sunrise (for the day or night icon) */
} ha_hourly_t;

esp_err_t ha_hourly_refresh(const app_settings_t *cfg);
const ha_hourly_t *ha_hourly_acquire(void);   /* NULL if none / over an hour old */
void ha_hourly_release(void);

/* True while the latest attempt at any history graph's data failed or came
 * back cut off - the weather panel shows a warning icon then. */
bool ha_graphs_incomplete(void);

/* Internal: called from ha_weather_refresh(). */
esp_err_t ha_weather_details_refresh(const app_settings_t *cfg);

#ifdef __cplusplus
}
#endif
