#pragma once
/* Fetches events for all enabled calendars in cfg and republishes them
 * into the event_store. Call this once at boot and then periodically
 * (see main.c's refresh task) - each call does a full re-fetch and
 * atomic swap, so a failed/partial fetch never corrupts what's on
 * screen. */

#include <stdbool.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* window_past_days/window_future_days control how much of the calendar
 * is pulled around "now" - wide enough to cover a full month view plus
 * next/prev paging without re-fetching on every tap.
 *
 * *out_all_ok is set to whether *every* configured calendar's fetch
 * succeeded this cycle - false if even one failed, even though that's
 * still an ESP_OK return (see gcal_refresh_all()'s own comment on why a
 * partial fetch still gets stored rather than discarded). Callers use
 * this to still surface a warning to the user when e.g. 2 of 3 calendars
 * came back - a real, ongoing problem with that one calendar that a
 * silent "refresh complete" would otherwise hide indefinitely. */
esp_err_t gcal_refresh_all(const app_settings_t *cfg, int window_past_days, int window_future_days,
                            bool *out_all_ok);

#ifdef __cplusplus
}
#endif
