#pragma once
/*
 * Up to two news feeds (RSS 2.0 or Atom), each from a URL set on the config
 * page. Shown from the two bottom-corner icons on the ambient clock (see
 * calendar_ui/ui_feed.c): heading and plain text only, no images.
 *
 * Fetched from the background network task, one feed at a time, every
 * REFRESH_MIN minutes (a failure is retried after 5). At most RSS_MAX_ITEMS
 * items per feed are kept (~6.6KB each, PSRAM). The reader stops at 160KB,
 * so a huge feed costs time, not memory.
 */

#include "app_settings.h"
#include "esp_err.h"
#include "rss_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSS_FEED_COUNT 2

/* Fetches whichever configured feeds are due. Call from the background
 * network task (blocks for the duration of the HTTPS requests). */
esp_err_t rss_feed_refresh(const app_settings_t *cfg);

/* Feed `idx` (0 or 1), or NULL if it has no data yet. Holds a lock until
 * rss_feed_release() - keep it brief, and always release, NULL or not. */
const rss_feed_t *rss_feed_acquire(int idx);
void rss_feed_release(void);

#ifdef __cplusplus
}
#endif
