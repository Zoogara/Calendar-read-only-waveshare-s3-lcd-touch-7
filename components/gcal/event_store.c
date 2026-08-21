#include "event_store.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* EVENT_STORE_MAX_EVENTS * sizeof(gcal_event_t) is tens of KB - park it
 * in PSRAM (there's 8MB of it) rather than eating into the much
 * smaller, more contended internal DRAM that Wi-Fi/TLS/LVGL need. */
static gcal_event_t *s_events;
static int s_count;
static time_t s_last_refresh;
static SemaphoreHandle_t s_lock;

void event_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_events == NULL) {
        s_events = heap_caps_malloc(EVENT_STORE_MAX_EVENTS * sizeof(gcal_event_t), MALLOC_CAP_SPIRAM);
        if (s_events == NULL) {
            /* Fall back to internal RAM rather than leaving the store
             * unusable - shouldn't happen given the board's 8MB PSRAM. */
            ESP_LOGW("event_store", "PSRAM allocation failed, falling back to internal RAM");
            s_events = calloc(EVENT_STORE_MAX_EVENTS, sizeof(gcal_event_t));
        }
    }
}

static int cmp_start(const void *a, const void *b)
{
    const gcal_event_t *ea = a, *eb = b;
    if (ea->start < eb->start) return -1;
    if (ea->start > eb->start) return 1;
    return 0;
}

void event_store_replace_all(const gcal_event_t *events, int count)
{
    if (count > EVENT_STORE_MAX_EVENTS) {
        count = EVENT_STORE_MAX_EVENTS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_events, events, count * sizeof(gcal_event_t));
    s_count = count;
    qsort(s_events, s_count, sizeof(gcal_event_t), cmp_start);
    xSemaphoreGive(s_lock);
}

int event_store_copy_range(time_t range_start, time_t range_end, gcal_event_t *out, int max_out)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max_out; i++) {
        const gcal_event_t *e = &s_events[i];
        if (e->end > range_start && e->start < range_end) {
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

time_t event_store_last_refresh(void)
{
    return s_last_refresh;
}

void event_store_mark_refreshed(void)
{
    time(&s_last_refresh);
}
