#pragma once
/*
 * The small parsing and bookkeeping behind the radar page (pure C, no ESP-IDF - host-tested).
 *
 * Home Assistant's radar template sensor (sensor.template_weather_display_radar) carries, as attributes,
 * url1..url6 (RainViewer frame URLs, url1 the newest), timestamp1..timestamp6 (the frames' local times as
 * HA formatted them, e.g. "1:10 PM"), and for the satellite image `urls` and `timestamps`; the satellite's
 * exact UTC time is the state of sensor.himawari_latest ("2026-11-22T01:20:00Z"). HA downloads each frame
 * and serves it as /local/radar/<frame id>.png, the id being the last part of the URL's /radar/<id>/ path.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_FRAMES   6
#define RADAR_ID_MAX   32

/* "https://tilecache.rainviewer.com/v2/radar/34ec6b97a411/256/7/-36.05/146.45/1/1_1.png" -> "34ec6b97a411".
 * False if there is no /radar/<id>/ part or the id is empty, too long, or not letters and digits only (it
 * becomes part of a URL and a file name, so it is checked). */
bool radar_id_from_url(const char *url, char *id, size_t id_sz);

/* "1:10 PM", "12:20 AM", "13:05" or "7:30" (no AM/PM = 24-hour) -> hour 0-23 and minute. */
bool radar_parse_clock(const char *s, int *hour24, int *minute);

/* "2026-11-22T01:20:00Z" -> seconds since the epoch (UTC). Checks ranges, leap years included. */
bool radar_parse_iso_utc(const char *s, time_t *out);

/* The time of a frame given only as a clock reading from today: the most recent moment at or before `now`
 * (plus a little slack for clocks that disagree) with that hour and minute in local time - so a "11:50 PM"
 * frame seen at 12:05 AM is yesterday's. */
time_t radar_frame_time(time_t now, int hour24, int minute);

/* Which of the frames already held can be kept when the list changes: for each of the n_new ids, the index
 * into the n_old held ids with the same id, or -1 if it has to be fetched. Returns how many are -1. */
int radar_plan_frames(const char old_ids[][RADAR_ID_MAX], int n_old, const char new_ids[][RADAR_ID_MAX], int n_new,
                      int reuse_index[]);

/* Puts frames in chronological order, oldest first, so the page can never show them out of order whatever
 * order Home Assistant lists them in. times[] holds each frame's time and order[] receives the indices of
 * the frames to show: ascending by time, a frame whose id or time repeats an earlier one dropped. Returns how
 * many are in order[] (at most n). */
int radar_order_frames(const time_t times[], const char ids[][RADAR_ID_MAX], int n, int order[]);

/* When to look for a new frame: RainViewer publishes one every 10 minutes (on the UTC :00, :10 ...) and it
 * reaches HA - and its /local copy - about a minute later, so the next check is at 2 minutes past the next
 * 10-minute mark after `now`. A check that finds nothing new is repeated every RADAR_RETRY_S, up to
 * RADAR_RETRIES times, before waiting for the next mark. */
#define RADAR_CHECK_OFFSET_S (2 * 60)
#define RADAR_RETRY_S        45
#define RADAR_RETRIES        6
time_t radar_next_check(time_t now);

#ifdef __cplusplus
}
#endif
