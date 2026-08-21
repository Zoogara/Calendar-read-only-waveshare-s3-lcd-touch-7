#pragma once
/* Thread-safe in-RAM store of the merged, multi-calendar event list.
 * The Google Calendar fetch task writes a fresh snapshot in one shot
 * (event_store_replace_all); the UI reads filtered copies out of it. */

#include <stddef.h>
#include <time.h>
#include "gcal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Both this and gcal_client.c's MAX_FETCH_EVENTS need to move together -
 * they're conceptually the same cap (how many events one fetch cycle can
 * carry end to end), just expressed in two files. Headroom is cheap now
 * that both buffers live in PSRAM (8MB) rather than internal RAM - 1000
 * events comfortably covers a much wider fetch window (e.g. 300 days
 * future) across several calendars without silently truncating, at a
 * PSRAM cost of well under 200KB. */
#define EVENT_STORE_MAX_EVENTS 1000

void event_store_init(void);

/* Atomically replaces the whole event set (sorted by start time
 * internally). Called after each successful refresh cycle. */
void event_store_replace_all(const gcal_event_t *events, int count);

/* Copies events overlapping [range_start, range_end) into out (caller
 * allocates, size max_out), sorted by start time. Returns count copied. */
int event_store_copy_range(time_t range_start, time_t range_end, gcal_event_t *out, int max_out);

/* Copies up to max_out events starting at/after `now`, soonest first. */
int event_store_copy_upcoming(time_t now, gcal_event_t *out, int max_out);

/* Epoch seconds of the last successful refresh, or 0 if none yet. */
time_t event_store_last_refresh(void);
void event_store_mark_refreshed(void);

#ifdef __cplusplus
}
#endif
