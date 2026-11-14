#include "event_store.h"
#include <stdio.h>
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

/* Text pools for event details, swapped together with the event buffers. Offset 0 is never a valid
 * detail (it means "none"), so each pool starts at length 1. s_gen is the generation of the
 * published pool, s_scratch_gen the one being filled; gen 0 is skipped (it means "never set"). */
static char *s_pool;
static char *s_pool_scratch;
static size_t s_pool_len = 1;
static size_t s_pool_scratch_len = 1;
static uint16_t s_gen = 1;
static uint16_t s_scratch_gen = 2;
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
    if (s_pool == NULL) {
        s_pool = heap_caps_malloc(EVENT_STORE_DETAIL_POOL_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_pool_scratch == NULL) {
        s_pool_scratch = heap_caps_malloc(EVENT_STORE_DETAIL_POOL_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_pool == NULL || s_pool_scratch == NULL) {
        /* The calendar still works; events just have no details box text. */
        ESP_LOGW("event_store", "PSRAM allocation for the event detail pools failed - no details");
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
    /* A fresh pool for this sync, one generation on from the published one. */
    s_pool_scratch_len = 1;
    s_scratch_gen = (uint16_t)(s_gen + 1);
    if (s_scratch_gen == 0) {
        s_scratch_gen = 1;
    }
    return (s_events != NULL) ? s_scratch : NULL;
}

uint32_t event_store_detail_add(const char *location, const char *description, uint16_t *gen_out)
{
    if (location == NULL) {
        location = "";
    }
    if (description == NULL) {
        description = "";
    }
    if (s_pool_scratch == NULL || (location[0] == '\0' && description[0] == '\0')) {
        return 0;
    }
    size_t ll = strlen(location), dl = strlen(description);
    if (s_pool_scratch_len + ll + 1 + dl + 1 > EVENT_STORE_DETAIL_POOL_BYTES) {
        return 0;
    }
    uint32_t off = (uint32_t)s_pool_scratch_len;
    memcpy(s_pool_scratch + off, location, ll + 1);
    memcpy(s_pool_scratch + off + ll + 1, description, dl + 1);
    s_pool_scratch_len += ll + 1 + dl + 1;
    if (gen_out != NULL) {
        *gen_out = s_scratch_gen;
    }
    return off;
}

bool event_store_detail_get(uint32_t offset, uint16_t gen, char *location, size_t loc_sz,
                            char *description, size_t desc_sz)
{
    if (location != NULL && loc_sz > 0) {
        location[0] = '\0';
    }
    if (description != NULL && desc_sz > 0) {
        description[0] = '\0';
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (offset != 0 && gen == s_gen && s_pool != NULL && offset < s_pool_len) {
        const char *loc = s_pool + offset;
        const char *desc = loc + strlen(loc) + 1;
        if (desc < s_pool + s_pool_len) {
            if (location != NULL && loc_sz > 0) {
                snprintf(location, loc_sz, "%s", loc);
            }
            if (description != NULL && desc_sz > 0) {
                snprintf(description, desc_sz, "%s", desc);
            }
            ok = true;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
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
    char *old_pool = s_pool;                 /* the text pools swap with them */
    s_pool = s_pool_scratch;
    s_pool_len = s_pool_scratch_len;
    s_pool_scratch = old_pool;
    s_gen = s_scratch_gen;
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
            gcal_event_t ev = s_events[i];
            if (ev.detail != 0 && ev.detail_gen == s_gen && ev.detail < s_pool_len && s_pool != NULL) {
                /* Its text lives in the published pool, which the next publish retires: re-add it to
                 * the pool this sync is filling (gen and offset change; the strings don't). */
                const char *loc = s_pool + ev.detail;
                const char *desc = loc + strlen(loc) + 1;
                ev.detail = event_store_detail_add(loc, desc, &ev.detail_gen);
            } else {
                ev.detail = 0;
            }
            out[(*inout_count)++] = ev;
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
