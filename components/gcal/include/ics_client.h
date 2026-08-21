#pragma once
/* Plain HTTPS ICS (iCalendar, RFC 5545) feed fetching - for calendars that
 * can't be shared with the service account at all (some providers mark
 * calendars as unshareable even once imported into Google Calendar), as
 * an alternative to gcal_client.c's Google Calendar API path. See
 * app_cal_source_t in app_settings.h for how a calendar picks one or the
 * other.
 *
 * Scope: only non-recurring VEVENTs are imported - a VEVENT with an
 * RRULE is skipped rather than showing just its first occurrence (which
 * would be misleadingly incomplete). DTSTART/DTEND are read either as
 * UTC ("...Z" suffix) or as a bare date (VALUE=DATE, all-day events); a
 * TZID-qualified local time is treated as the device's own configured
 * local time rather than the named zone, since expanding arbitrary IANA
 * timezone rules isn't implemented - fine for a calendar in the same
 * timezone as the display, not correct for one that isn't. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_http_client.h"
#include "app_settings.h"
#include "gcal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same shape/contract as fetch_one_calendar() in gcal_client.c (not
 * exposed there - this is the ICS-source counterpart gcal_refresh_all()
 * calls instead, based on cal->source). client is reused, not created
 * here - the caller is responsible for making sure no stale Authorization
 * header (from a Google Calendar API request) is still set on it, since
 * that token has no business being sent to an arbitrary ICS host. */
bool ics_client_fetch(esp_http_client_handle_t client, const app_calendar_cfg_t *cal,
                       uint8_t calendar_index, time_t time_min, time_t time_max,
                       gcal_event_t *out, int *inout_count, int max_out);

#ifdef __cplusplus
}
#endif
