#include "event_store.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Two event buffers, both in PSRAM for the device's lifetime: s_events is
 * what the UI reads, s_scratch is what the next sync fills. Publishing a
 * sync swaps the two pointers under the lock (see event_store_publish()),
 * so the UI is never blocked behind a 2000-event copy or sort, and there's
 * no ~336KB allocate/free every cycle. Internal RAM is the scarce resource
 * here; PSRAM has room for both. */
static gcal_event_t *s_events;
static gcal_event_t *s_scratch;
static int s_count;
static time_t s_last_refresh;
static SemaphoreHandle_t s_lock;

void event_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    const size_t bytes = EVENT_STORE_MAX_EVENTS * sizeof(gcal_event_t);
    if (s_events == NULL) {
        s_events = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    if (s_scratch == NULL) {
        s_scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    if (s_events == NULL || s_scratch == NULL) {
        /* Not falling back to internal RAM: at ~336KB per buffer it
         * wouldn't fit anyway. With either missing, every sync fails with
         * ESP_ERR_NO_MEM (logged) and the calendar stays empty. */
        ESP_LOGE("event_store", "PSRAM allocation of %u bytes failed - calendar sync disabled",
                 (unsigned)bytes);
    }
}

/* Start time, then calendar, then end - a total order, so the same set of
 * events always comes out in the same order (qsort isn't stable). */
static int cmp_event(const void *a, const void *b)
{
    const gcal_event_t *ea = a, *eb = b;
    if (ea->start != eb->start) return ea->start < eb->start ? -1 : 1;
    if (ea->calendar_index != eb->calendar_index) return ea->calendar_index < eb->calendar_index ? -1 : 1;
    if (ea->end != eb->end) return ea->end < eb->end ? -1 : 1;
    return 0;
}

gcal_event_t *event_store_begin_update(void)
{
    return (s_events != NULL) ? s_scratch : NULL;
}

void event_store_publish(int count)
{
    if (s_scratch == NULL || s_events == NULL) {
        return;
    }
    if (count > EVENT_STORE_MAX_EVENTS) {
        count = EVENT_STORE_MAX_EVENTS;
    }
    /* Sort before taking the lock - only the syncing task touches s_scratch. */
    qsort(s_scratch, count, sizeof(gcal_event_t), cmp_event);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    gcal_event_t *old = s_events;
    s_events = s_scratch;
    s_count = count;
    s_scratch = old;   /* the next sync's scratch */
    xSemaphoreGive(s_lock);
}

int event_store_copy_range(time_t range_start, time_t range_end, gcal_event_t *out, int max_out)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max_out; i++) {
        const gcal_event_t *e = &s_events[i];
        if (e->start >= range_end) {
            break;   /* sorted by start - nothing later can overlap */
        }
        if (e->end > range_start) {
            out[n++] = *e;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int event_store_copy_upcoming(time_t now, gcal_event_t *out, int max_out)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max_out; i++) {
        const gcal_event_t *e = &s_events[i];
        if (e->end > now) {
            out[n++] = *e;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int event_store_copy_calendar(uint8_t calendar_index, gcal_event_t *out, int max_out, int *inout_count)
{
    int added = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && *inout_count < max_out; i++) {
        if (s_events[i].calendar_index == calendar_index) {
            out[(*inout_count)++] = s_events[i];
            added++;
        }
    }
    xSemaphoreGive(s_lock);
    return added;
}

time_t event_store_last_refresh(void)
{
    return s_last_refresh;
}

void event_store_mark_refreshed(void)
{
    time(&s_last_refresh);
}
