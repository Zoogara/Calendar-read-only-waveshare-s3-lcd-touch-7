#pragma once
/* Shared event model for the Google Calendar client + UI. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCAL_MAX_SUMMARY 96

/* The longest location and description kept for an event's details box. They are NOT stored in
 * gcal_event_t (two buffers of 2000 events would cost ~1.9MB of PSRAM): they sit in the event
 * store's text pool, found through gcal_event_t::detail - see event_store_detail_add()/get(). */
#define GCAL_MAX_LOCATION     96
#define GCAL_MAX_DESCRIPTION  400
typedef struct {
    char summary[GCAL_MAX_SUMMARY];
    time_t start;          /* UTC epoch seconds */
    time_t end;             /* UTC epoch seconds (exclusive) */
    bool all_day;
    uint32_t color;         /* 0xRRGGBB, copied from the owning calendar's
                                configured colour at fetch time */
    char calendar_label[40];
    uint8_t calendar_index; /* index into app_settings_t.calendars[] */
    uint16_t detail_gen;    /* which sync's text pool detail points into (see event_store.h) */
    uint32_t detail;        /* byte offset of this event's location+description in that pool; 0 = none */
} gcal_event_t;

#ifdef __cplusplus
}
#endif
