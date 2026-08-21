#pragma once
/* Shared event model for the Google Calendar client + UI. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCAL_MAX_SUMMARY 96

typedef struct {
    char summary[GCAL_MAX_SUMMARY];
    time_t start;          /* UTC epoch seconds */
    time_t end;             /* UTC epoch seconds (exclusive) */
    bool all_day;
    uint32_t color;         /* 0xRRGGBB, copied from the owning calendar's
                                configured colour at fetch time */
    char calendar_label[40];
    uint8_t calendar_index; /* index into app_settings_t.calendars[] */
} gcal_event_t;

#ifdef __cplusplus
}
#endif
