#include "gcal_client.h"
#include "gcal.h"
#include "jwt_auth.h"
#include "event_store.h"
#include "ics_client.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gcal_client";

/* Must match EVENT_STORE_MAX_EVENTS (event_store.h) - see its comment for
 * why 1000 rather than the old 320. */
#define MAX_FETCH_EVENTS 1000

/* esp_http_client_perform() can return ESP_ERR_HTTP_EAGAIN even for a
 * blocking (non-async) client if a header/data read times out mid-transfer
 * - ESP-IDF's own esp_http_client.c comments say the caller should just
 * call perform() again on the same handle to pick up where it left off.
 * Seen in practice on a Wi-Fi link that occasionally drops/reconnects. */
static esp_err_t http_perform_with_retry(esp_http_client_handle_t client)
{
    esp_err_t err;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return err;
}

/* ---------------- small helpers ---------------- */

static void url_encode(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_sz; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            o += snprintf(out + o, out_sz - o, "%%%02X", *p);
        }
    }
    out[o] = '\0';
}

/* ESP-IDF's newlib doesn't provide timegm(), so convert UTC-wallclock
 * struct tm fields to a Unix timestamp ourselves. days_from_civil is
 * Howard Hinnant's constant-time civil-calendar algorithm. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t timegm_utc(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* Parses "YYYY-MM-DDTHH:MM:SS[.sss](Z|+HH:MM|-HH:MM)" -> UTC epoch. */
static bool parse_rfc3339_datetime(const char *s, time_t *out)
{
    struct tm tm = {0};
    int matched = sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d",
                          &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                          &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (matched != 6) {
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    /* Timezone designator starts right after "HH:MM:SS" (offset 19 from
     * the start of the string), possibly preceded by ".sss" fractional
     * seconds - skip past those first. */
    const char *tzpart = s + 19;
    if (*tzpart == '.') {
        tzpart = strpbrk(tzpart, "Z+-");
    }

    int tz_offset_s = 0;
    if (tzpart != NULL && *tzpart != '\0' && *tzpart != 'Z') {
        int sign = (*tzpart == '-') ? -1 : 1;
        int tz_h = 0, tz_m = 0;
        sscanf(tzpart + 1, "%2d:%2d", &tz_h, &tz_m);
        tz_offset_s = sign * (tz_h * 3600 + tz_m * 60);
    }

    /* timegm() treats tm as UTC wall-clock fields; the string's fields
     * are local-to-tz_offset, so UTC = wall_clock - offset. */
    *out = timegm_utc(&tm) - tz_offset_s;
    return true;
}

static bool parse_date_only(const char *s, time_t *out)
{
    struct tm tm = {0};
    if (sscanf(s, "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    /* All-day events' "date" fields are bare calendar dates with no
     * attached timezone - they mean the LOCAL day, not UTC midnight of
     * that date. Using timegm_utc() here (like parse_rfc3339_datetime does
     * for real dateTime values) anchored them to UTC midnight instead,
     * which for any timezone ahead of UTC pushed the event's end boundary
     * (Google's "end.date" is exclusive, i.e. the day after the last day
     * the event occurs) into the next LOCAL day too, making every all-day
     * event appear to span an extra day it didn't actually occur on. Local
     * midnight (mktime, using the TZ main.c sets at boot) matches how
     * ui_start_of_day() computes the day-cell boundaries these get
     * compared against in event_store_copy_range(). */
    tm.tm_isdst = -1;
    *out = mktime(&tm);
    return true;
}

struct http_resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    struct http_resp_buf *buf = (struct http_resp_buf *)evt->user_data;
    /* evt->data is already de-chunked payload during ON_DATA regardless of
     * transfer encoding when using esp_http_client_perform() - see the
     * matching comment in jwt_auth.c's http_event_handler for why
     * excluding chunked responses here silently drops response bodies. */
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf->len + evt->data_len + 1 > buf->cap) {
            size_t new_cap = (buf->len + evt->data_len + 1) * 2;
            /* MALLOC_CAP_SPIRAM - see the resp buffer's own allocation
             * comment in gcal_refresh_all(); this growth path needs the
             * same capability or a large response would start internal
             * and only partway migrate to PSRAM as it grows. */
            char *grown = heap_caps_realloc(buf->data, new_cap, MALLOC_CAP_SPIRAM);
            if (grown == NULL) {
                return ESP_FAIL;
            }
            buf->data = grown;
            buf->cap = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, evt->data_len);
        buf->len += evt->data_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

/* Google's fixed 11-colour "event colour" palette (colorId "1".."11"),
 * used when an individual event carries its own colorId that overrides
 * its calendar's default colour (cal->color, picked once at setup time -
 * an earlier attempt to also auto-sync *calendar*-level colours from
 * Google's calendarList needed a wider OAuth scope than this app's
 * intentionally read-only credential grants, so that part was reverted).
 * These are Google's own documented, stable event-colour hex values
 * (colors.event in the Calendar API's Colors resource), hardcoded here
 * rather than fetched since they never change and fetching them would be
 * one more request for a fixed 11-entry table. */
static uint32_t event_color_from_id(const char *color_id)
{
    static const uint32_t PALETTE[] = {
        0x7986CB, /* 1 Lavender */  0x33B679, /* 2 Sage */
        0x8E24AA, /* 3 Grape */     0xE67C73, /* 4 Flamingo */
        0xF6C026, /* 5 Banana */    0xF5511D, /* 6 Tangerine */
        0x039BE5, /* 7 Peacock */   0x616161, /* 8 Graphite */
        0x3F51B5, /* 9 Blueberry */ 0x0B8043, /* 10 Basil */
        0xD60000, /* 11 Tomato */
    };
    if (color_id == NULL) {
        return 0;
    }
    int idx = atoi(color_id);
    if (idx < 1 || idx > (int)(sizeof(PALETTE) / sizeof(PALETTE[0]))) {
        return 0;
    }
    return PALETTE[idx - 1];
}

/* True if the calendar's owner declined this event. We authenticate as a
 * service account that only has calendar-level read access (shared with
 * it, not invited to individual events), so it never appears in an
 * event's attendees list itself - there's no "self: true" entry to check
 * the way a real user's own client would. What we actually have is
 * cal->id, which for a primary personal calendar *is* the owner's email
 * address, so matching that against attendees[].email is the closest
 * equivalent: it correctly hides events the calendar owner declined on
 * their main calendar, and is harmlessly a no-op on secondary/shared
 * calendars that aren't a single person's mailbox (group.calendar.google.com
 * IDs, resource calendars, etc.) where no attendee will ever match. */
static bool event_declined_by_owner(cJSON *item, const char *owner_email)
{
    if (owner_email == NULL || owner_email[0] == '\0') {
        return false;
    }
    cJSON *attendees = cJSON_GetObjectItemCaseSensitive(item, "attendees");
    if (!cJSON_IsArray(attendees)) {
        return false;
    }
    cJSON *att;
    cJSON_ArrayForEach(att, attendees) {
        cJSON *email_j = cJSON_GetObjectItemCaseSensitive(att, "email");
        cJSON *status_j = cJSON_GetObjectItemCaseSensitive(att, "responseStatus");
        if (cJSON_IsString(email_j) && cJSON_IsString(status_j) &&
            strcasecmp(email_j->valuestring, owner_email) == 0) {
            return strcmp(status_j->valuestring, "declined") == 0;
        }
    }
    return false;
}

/* Fetches one calendar's events into out[], starting at *inout_count, up
 * to max_out total. Returns whether the fetch itself succeeded (a valid
 * response was parsed - zero events in a legitimately empty calendar
 * still counts as success); one bad/unreachable calendar is logged and
 * doesn't block the others, but the caller uses this to tell "nothing to
 * show" apart from "couldn't reach anything," so a total outage doesn't
 * blank out the display's last-known-good data.
 *
 * Follows nextPageToken across multiple requests (same reused client, so
 * this doesn't reintroduce TLS-session churn) - a single 250-event page
 * silently truncated any calendar busy enough to exceed that in the
 * fetch window, which for a shared/family calendar over ~74 days is a
 * real risk, not just theoretical. */
static bool fetch_one_calendar(esp_http_client_handle_t client, const app_calendar_cfg_t *cal,
                                uint8_t calendar_index, time_t time_min, time_t time_max,
                                gcal_event_t *out, int *inout_count, int max_out)
{
    ESP_LOGI(TAG, "[%s] fetching calendar id \"%s\"", cal->label, cal->id);

    char cal_id_enc[192];
    url_encode(cal->id, cal_id_enc, sizeof(cal_id_enc));

    struct tm tmv;
    char tmin_s[24], tmax_s[24];
    gmtime_r(&time_min, &tmv);
    strftime(tmin_s, sizeof(tmin_s), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    gmtime_r(&time_max, &tmv);
    strftime(tmax_s, sizeof(tmax_s), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    char tmin_enc[32], tmax_enc[32];
    url_encode(tmin_s, tmin_enc, sizeof(tmin_enc));
    url_encode(tmax_s, tmax_enc, sizeof(tmax_enc));

    char base_url[420];
    snprintf(base_url, sizeof(base_url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events"
             "?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime&maxResults=250",
             cal_id_enc, tmin_enc, tmax_enc);

    char page_token[256] = {0};
    int added = 0;
    bool any_page_ok = false;

    for (;;) {
        char url[1000]; /* base_url (up to 420) + "&pageToken=" + token_enc (up to 512) */
        if (page_token[0] != '\0') {
            char token_enc[512];
            url_encode(page_token, token_enc, sizeof(token_enc));
            snprintf(url, sizeof(url), "%s&pageToken=%s", base_url, token_enc);
        } else {
            snprintf(url, sizeof(url), "%s", base_url);
        }

        /* MALLOC_CAP_SPIRAM - same reasoning as the gcal_event_t buffer
         * below: this is a pure scratch buffer for accumulating the JSON
         * response text before parsing, no DMA/hardware constraint, and
         * plain malloc()/realloc() (see http_event_handler() above, fixed
         * to match) would otherwise contend with mbedtls's internal-RAM-
         * only TLS handshake buffers for the same small pool - especially
         * costly here since a full 250-event page can grow this well past
         * the CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL threshold anyway. */
        struct http_resp_buf resp = {.data = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM), .len = 0, .cap = 2048};
        if (resp.data == NULL) {
            break;
        }
        resp.data[0] = '\0';

        esp_http_client_set_url(client, url);
        esp_http_client_set_user_data(client, &resp);

        esp_err_t err = http_perform_with_retry(client);
        int status = esp_http_client_get_status_code(client);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[%s] request failed: %s", cal->label, esp_err_to_name(err));
            free(resp.data);
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "[%s] HTTP %d: %.200s", cal->label, status, resp.data);
            free(resp.data);
            break;
        }

        cJSON *root = cJSON_Parse(resp.data);
        free(resp.data);
        if (root == NULL) {
            ESP_LOGW(TAG, "[%s] bad JSON in response", cal->label);
            break;
        }
        any_page_ok = true;

        cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        if (cJSON_IsArray(items)) {
            cJSON *item;
            cJSON_ArrayForEach(item, items) {
                if (*inout_count >= max_out) {
                    break;
                }
                cJSON *status_j = cJSON_GetObjectItemCaseSensitive(item, "status");
                if (cJSON_IsString(status_j) && strcmp(status_j->valuestring, "cancelled") == 0) {
                    continue;
                }
                if (event_declined_by_owner(item, cal->id)) {
                    continue;
                }
                cJSON *start_j = cJSON_GetObjectItemCaseSensitive(item, "start");
                cJSON *end_j = cJSON_GetObjectItemCaseSensitive(item, "end");
                if (!cJSON_IsObject(start_j) || !cJSON_IsObject(end_j)) {
                    continue;
                }

                gcal_event_t ev = {0};
                cJSON *summary_j = cJSON_GetObjectItemCaseSensitive(item, "summary");
                strncpy(ev.summary,
                        (cJSON_IsString(summary_j) && summary_j->valuestring) ? summary_j->valuestring : "(No title)",
                        sizeof(ev.summary) - 1);

                cJSON *sdate = cJSON_GetObjectItemCaseSensitive(start_j, "date");
                cJSON *sdt = cJSON_GetObjectItemCaseSensitive(start_j, "dateTime");
                cJSON *edate = cJSON_GetObjectItemCaseSensitive(end_j, "date");
                cJSON *edt = cJSON_GetObjectItemCaseSensitive(end_j, "dateTime");

                bool ok = true;
                if (cJSON_IsString(sdt) && cJSON_IsString(edt)) {
                    ev.all_day = false;
                    ok = parse_rfc3339_datetime(sdt->valuestring, &ev.start) &&
                         parse_rfc3339_datetime(edt->valuestring, &ev.end);
                } else if (cJSON_IsString(sdate) && cJSON_IsString(edate)) {
                    ev.all_day = true;
                    ok = parse_date_only(sdate->valuestring, &ev.start) &&
                         parse_date_only(edate->valuestring, &ev.end);
                } else {
                    ok = false;
                }
                if (!ok) {
                    continue;
                }

                /* colorId is confirmed to be unconditionally omitted by
                 * Google for a read-only, ACL-shared service-account
                 * credential like this one - verified against both
                 * events.list() and events.get() (a clean HTTP 200, full
                 * event details, colorId simply never present) for an
                 * event whose owner-side Google UI shows an explicit
                 * colour ("Tomato") selected. This override still costs
                 * nothing to keep - it just won't fire under the current
                 * auth model, and would need real per-user OAuth sign-in
                 * (not a service account) to ever see this field. */
                cJSON *color_id_j = cJSON_GetObjectItemCaseSensitive(item, "colorId");
                uint32_t override_color = cJSON_IsString(color_id_j)
                                               ? event_color_from_id(color_id_j->valuestring)
                                               : 0;
                ev.color = override_color != 0 ? override_color : cal->color;
                ev.calendar_index = calendar_index;
                strncpy(ev.calendar_label, cal->label, sizeof(ev.calendar_label) - 1);

                out[*inout_count] = ev;
                (*inout_count)++;
                added++;
            }
        }

        cJSON *next_token_j = cJSON_GetObjectItemCaseSensitive(root, "nextPageToken");
        bool has_next = cJSON_IsString(next_token_j) && next_token_j->valuestring[0] != '\0';
        if (has_next) {
            strncpy(page_token, next_token_j->valuestring, sizeof(page_token) - 1);
        }
        cJSON_Delete(root);

        if (!has_next || *inout_count >= max_out) {
            break;
        }
    }

    ESP_LOGI(TAG, "[%s] %d event(s)", cal->label, added);
    return any_page_ok;
}

/* Logged around every refresh attempt - kept (not just temporary debug)
 * because it's cheap and directly evidences the fix below: repeatedly
 * tearing a TLS session down and rebuilding it (esp_http_client_init()/
 * cleanup() per request) fragments the much smaller internal-SRAM pool
 * badly enough, over enough cycles, that mbedtls's RSA verification step
 * during certificate checking can no longer get a big-number allocation it
 * needs ("PK verify failed ... MBEDTLS_ERR_RSA_PUBLIC_FAILED +
 * MBEDTLS_ERR_MPI_ALLOC_FAILED" decoded from the esp-x509-crt-bundle log),
 * and the device is then stuck failing every refresh until rebooted. */
static void log_heap_state(const char *context)
{
    ESP_LOGI(TAG, "%s - heap: %u free / %u largest block, internal: %u free / %u largest block",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

esp_err_t gcal_refresh_all(const app_settings_t *cfg, int window_past_days, int window_future_days,
                            bool *out_all_ok)
{
    *out_all_ok = false;
    log_heap_state("refresh start");

    char token[1024];
    esp_err_t err = jwt_auth_get_token(cfg, token, sizeof(token));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not get access token, skipping refresh");
        return err;
    }

    time_t now;
    time(&now);
    time_t time_min = now - (time_t)window_past_days * 86400;
    time_t time_max = now + (time_t)window_future_days * 86400;

    /* MALLOC_CAP_SPIRAM, not a plain calloc() - this buffer (tens of KB at
     * MAX_FETCH_EVENTS) is alive for the ENTIRE fetch cycle, spanning
     * every calendar's TLS handshake. Plain malloc()/calloc() prefers
     * internal RAM whenever it's available (CONFIG_SPIRAM_MALLOC_
     * ALWAYSINTERNAL only forces the *small*-allocation case internal;
     * this buffer is well above that threshold and was landing on
     * internal RAM anyway under normal conditions), putting it in direct
     * contention with mbedtls's own internal-RAM-only handshake buffers
     * for the exact same pool, at the exact same time - a strong
     * candidate for the deep transient internal-RAM dips (down to single-
     * digit KB) seen mid-fetch during testing. event_store.c's permanent
     * storage already gets this right; this scratch buffer didn't. Each
     * entry is fully overwritten (zero-initialized locally, per struct)
     * before being copied in below, so skipping calloc()'s zeroing is
     * safe. */
    gcal_event_t *buf = heap_caps_malloc(MAX_FETCH_EVENTS * sizeof(gcal_event_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int count = 0;

    char auth_header[1100];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    /* One HTTP client shared across every calendar fetch this cycle (they
     * all hit the same host) instead of a fresh esp_http_client_init()/
     * cleanup() per calendar - see the log_heap_state() comment above for
     * why repeating that per-request was fragmenting internal SRAM badly
     * enough to eventually break TLS certificate verification. */
    esp_http_client_config_t http_cfg = {
        .url = "https://www.googleapis.com/calendar/v3",
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,     /* response buffer */
        /* The OAuth access token in our own Authorization header can run
         * well past esp_http_client's default 512-byte *request* buffer
         * (buffer_size_tx) once combined with the other headers it adds -
         * when that happens, http_header_generate_string() logs "Buffer
         * length is small to fit all the headers" and silently sends a
         * truncated/malformed request, which the server never responds to
         * (hangs until timeout rather than failing fast). This is a
         * request-side buffer, unrelated to and not fixed by buffer_size
         * (response) above. */
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(buf);
        ESP_LOGE(TAG, "failed to init HTTP client");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    /* Fetch every configured calendar regardless of its current
     * "enabled" flag - that flag is a UI-only show/hide toggle (tapping
     * a legend chip), so toggling it shouldn't require a network
     * round-trip. */
    int attempted = 0;
    int succeeded = 0;
    bool auth_header_set = true;
    for (int i = 0; i < cfg->calendar_count; i++) {
        const app_calendar_cfg_t *cal = &cfg->calendars[i];
        if (cal->id[0] == '\0') {
            continue;
        }
        attempted++;
        if (cal->source == APP_CAL_SOURCE_ICS) {
            /* The Google OAuth bearer token has no business going to an
             * arbitrary third-party ICS host - strip it before this
             * request and restore it before the next Google one. */
            if (auth_header_set) {
                esp_http_client_delete_header(client, "Authorization");
                auth_header_set = false;
            }
            if (ics_client_fetch(client, cal, (uint8_t)i, time_min, time_max, buf, &count, MAX_FETCH_EVENTS)) {
                succeeded++;
            }
        } else {
            if (!auth_header_set) {
                esp_http_client_set_header(client, "Authorization", auth_header);
                auth_header_set = true;
            }
            if (fetch_one_calendar(client, cal, (uint8_t)i, time_min, time_max, buf, &count, MAX_FETCH_EVENTS)) {
                succeeded++;
            }
        }
    }

    esp_http_client_cleanup(client);

    /* A cycle where every configured calendar failed (e.g. a Wi-Fi/TLS
     * blip - seen in practice hitting all of them in the same cycle, not
     * just one) is a failed refresh, not "0 events" - replacing the store
     * with an empty result here would blank out the display's
     * last-known-good data over a purely transient network hiccup that
     * the next scheduled cycle would likely recover from on its own. If
     * even one calendar came back, still store what did succeed rather
     * than discarding it because a sibling calendar failed. */
    if (succeeded == 0 && attempted > 0) {
        free(buf);
        ESP_LOGW(TAG, "refresh failed: 0 of %d calendar(s) reachable - keeping last-known data", attempted);
        log_heap_state("refresh failed");
        return ESP_FAIL;
    }

    event_store_replace_all(buf, count);
    event_store_mark_refreshed();
    free(buf);

    *out_all_ok = (succeeded == attempted);
    if (!*out_all_ok) {
        ESP_LOGW(TAG, "refresh partially failed: only %d of %d calendar(s) reachable", succeeded, attempted);
    }
    ESP_LOGI(TAG, "refresh complete: %d event(s) across %d of %d calendar(s)", count, succeeded, attempted);
    log_heap_state("refresh ok");
    return ESP_OK;
}
