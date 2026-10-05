#pragma once
/* Thread-safe in-RAM store of the merged, multi-calendar event list.
 * The calendar sync fills a scratch buffer (event_store_begin_update())
 * and publishes it in one step (event_store_publish()); the UI reads
 * filtered copies out of the published set. */

#include <stddef.h>
#include <time.h>
#include "gcal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many events one sync can carry end to end, across all calendars.
 * 2000 (raised from 1000, 2026-10-05) leaves room for the widest fetch
 * window the config page allows (90 days back, 365 ahead) across several
 * busy calendars - ~213 events over the default 14+60 days, scaled to a
 * year, came close to 1000. sizeof(gcal_event_t) is 168 bytes, so each of
 * the store's two PSRAM buffers (published + next-sync scratch) is ~336KB,
 * ~672KB together, permanently allocated - out of ~4MB of PSRAM free. */
#define EVENT_STORE_MAX_EVENTS 2000

void event_store_init(void);

/* The buffer (EVENT_STORE_MAX_EVENTS entries) for the next sync to fill,
 * or NULL if the store couldn't be allocated. Only one sync at a time may
 * use it - the calendar sync task is the only writer. Reading the
 * published set (e.g. event_store_copy_calendar()) while filling it is
 * fine: they're separate buffers. */
gcal_event_t *event_store_begin_update(void);

/* Makes the first count events of the begin_update() buffer the published
 * set: sorts them (start, then calendar, then end) without holding the
 * lock, then swaps buffers under it - readers never wait on the copy or
 * the sort. The old published buffer becomes the next sync's scratch. */
void event_store_publish(int count);

/* Copies events overlapping [range_start, range_end) into out (caller
 * allocates, size max_out), sorted by start time. Returns count copied. */
int event_store_copy_range(time_t range_start, time_t range_end, gcal_event_t *out, int max_out);

/* Copies up to max_out events that aren't over yet (end > now, NOT
 * start >= now - an event already under way still qualifies, which is
 * exactly what ui_upnext.c's "Up next" list wants: keep showing
 * something currently happening rather than dropping it right at its own
 * start time), sorted by start time, soonest first. A caller that only
 * wants events that haven't started yet (e.g. ui_reminder.c) has to
 * check `start > now` itself - this function deliberately doesn't. */
int event_store_copy_upcoming(time_t now, gcal_event_t *out, int max_out);

/* Appends every currently-stored event tagged with the given
 * calendar_index into out[] starting at *inout_count (bounded by
 * max_out), advancing *inout_count and returning how many were added.
 * Used by gcal_refresh_all() to carry a "Daily" calendar's existing
 * events forward on the cycles where it deliberately isn't re-fetched -
 * without this, event_store_publish()'s wholesale replace would drop
 * them. */
int event_store_copy_calendar(uint8_t calendar_index, gcal_event_t *out, int max_out, int *inout_count);

/* Epoch seconds of the last successful refresh, or 0 if none yet. */
time_t event_store_last_refresh(void);
void event_store_mark_refreshed(void);

#ifdef __cplusplus
}
#endif
