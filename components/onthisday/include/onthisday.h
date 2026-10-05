#pragma once
/*
 * "On this day in history" for today's local date, from Wikipedia's REST
 * API (feed/onthisday/selected/MM/DD - the editor-curated list, about 15-20
 * entries). Shown when the date on the ambient clock is tapped (see
 * calendar_ui/ui_history.c).
 *
 * Fetched at most once per local day, after a calendar sync, and kept in
 * PSRAM. The response is ~190KB (it carries full summaries of every linked
 * article), all of which is received and parsed in PSRAM; only the year, a
 * heading (the main linked article's title) and the one-line description of
 * each entry are kept, ~10KB.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONTHISDAY_MAX_ENTRIES 24

typedef struct {
    int16_t year;      /* negative for BC */
    char title[80];    /* UTF-8, may be empty */
    char text[320];    /* UTF-8 */
} onthisday_entry_t;

typedef struct {
    int month;         /* 1-12 */
    int day;           /* 1-31 */
    int count;
    onthisday_entry_t entries[ONTHISDAY_MAX_ENTRIES];
} onthisday_t;

/* Fetches today's list if it isn't already held, retrying a failure at most
 * every 30 minutes. Call from the background network task (blocks for the
 * duration of an HTTPS request). Needs the system clock set. */
esp_err_t onthisday_refresh(void);

/* Today's list, or NULL if there isn't one yet. Holds a lock until
 * onthisday_release() - keep it brief (it blocks the next publish), and
 * always release, NULL or not. */
const onthisday_t *onthisday_acquire(void);
void onthisday_release(void);

#ifdef __cplusplus
}
#endif
