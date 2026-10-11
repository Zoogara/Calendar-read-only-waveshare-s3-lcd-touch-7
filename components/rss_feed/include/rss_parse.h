#pragma once
/*
 * Pure-C RSS 2.0 / Atom reader: no ESP-IDF calls, so it can be tested on a
 * computer against real feeds (see tools/ if that is ever needed again).
 * Keeps only each item's heading and a short plain-text body.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSS_MAX_ITEMS  15
#define RSS_TITLE_LEN  120
#define RSS_TEXT_LEN   320
#define RSS_NAME_LEN   48
/* Scratch space rss_parse() needs from the caller (an item's raw text is
 * decoded here before its tags are stripped). */
#define RSS_SCRATCH_LEN 4096

typedef struct {
    char title[RSS_TITLE_LEN];   /* UTF-8, plain text */
    char text[RSS_TEXT_LEN];     /* UTF-8, plain text, may be empty */
} rss_item_t;

typedef struct {
    char name[RSS_NAME_LEN];     /* the feed's own title, may be empty */
    int count;
    rss_item_t items[RSS_MAX_ITEMS];
} rss_feed_t;

/* Parses `len` bytes of XML (NUL-terminated at xml[len]) into `out`, up to
 * RSS_MAX_ITEMS items. A truncated document still yields the items that came
 * through whole. `scratch` is RSS_SCRATCH_LEN bytes. Returns the item
 * count. */
int rss_parse(const char *xml, size_t len, rss_feed_t *out, char *scratch);

#ifdef __cplusplus
}
#endif
