#include "ics_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ics_client";

/* ---------------- small helpers duplicated from gcal_client.c ----------------
 * Same reasoning as config_web.c duplicating prov_web.c's form helpers -
 * these are small and self-contained, not worth a shared internal header
 * for. */

struct http_resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

/* No event_handler of our own here - the shared client's was already set
 * once by gcal_refresh_all() (gcal_client.c's http_event_handler, which
 * accumulates into whatever user_data esp_http_client_set_user_data()
 * points at - struct http_resp_buf above matches that shape). */

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

/* ESP-IDF's newlib doesn't provide timegm() - same Howard Hinnant
 * constant-time civil-calendar algorithm as gcal_client.c's version. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t ics_timegm_utc(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* ---------------- ICS-specific parsing ---------------- */

/* RFC 5545 line unfolding + CRLF normalization, in place: a "\r\n" is
 * just a line break (collapsed to "\n"), but a "\n" immediately followed
 * by a single space/tab is a *folded* continuation of the previous
 * logical line - drop that newline+whitespace pair to rejoin it. Without
 * this, a long SUMMARY/DTSTART/etc. that a server wrapped across multiple
 * physical lines would parse as garbage or get silently truncated at the
 * fold point. */
static void normalize_and_unfold(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\r' && r[1] == '\n') {
            r += 2;
            *w++ = '\n';
        } else if (r[0] == '\n' && (r[1] == ' ' || r[1] == '\t')) {
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* True if `line` is the property `name` (case-insensitive), i.e. starts
 * with "NAME:" or "NAME;...:" - if so, *out_value points at the text
 * right after the first ':' (still ICS-escaped, see ics_unescape_text). */
static bool prop_match(const char *line, const char *name, const char **out_value)
{
    size_t nlen = strlen(name);
    if (strncasecmp(line, name, nlen) != 0) {
        return false;
    }
    if (line[nlen] != ':' && line[nlen] != ';') {
        return false;
    }
    const char *colon = strchr(line + nlen, ':');
    if (colon == NULL) {
        return false;
    }
    *out_value = colon + 1;
    return true;
}

/* Substring search restricted to the "NAME;params" portion of a line
 * (before the first ':') - used to look for e.g. "VALUE=DATE" without
 * false-matching something that happens to appear in the value itself. */
static bool params_contain(const char *line, const char *needle)
{
    const char *colon = strchr(line, ':');
    size_t prefix_len = colon ? (size_t)(colon - line) : strlen(line);
    size_t needle_len = strlen(needle);
    if (needle_len > prefix_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= prefix_len; i++) {
        if (strncmp(line + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Un-escapes ICS TEXT value escaping (backslash-comma/semicolon/backslash,
 * "\n"/"\N" for newline - turned into a space here since our chip/block
 * labels are single-line). */
static void ics_unescape_text(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < out_sz; p++) {
        if (*p == '\\' && p[1] != '\0') {
            char n = p[1];
            out[o++] = (n == 'n' || n == 'N') ? ' ' : n;
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

typedef enum { ICS_DT_ALLDAY, ICS_DT_UTC, ICS_DT_LOCAL } ics_dt_mode_t;

static bool parse_ics_datetime(const char *value, ics_dt_mode_t mode, time_t *out)
{
    struct tm tm = {0};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (mode == ICS_DT_ALLDAY) {
        if (sscanf(value, "%4d%2d%2d", &y, &mo, &d) != 3) {
            return false;
        }
    } else {
        if (sscanf(value, "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) < 3) {
            return false;
        }
    }
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
    *out = (mode == ICS_DT_UTC) ? ics_timegm_utc(&tm) : mktime(&tm);
    return true;
}

static ics_dt_mode_t dt_mode_for_line(const char *line, const char *value)
{
    if (params_contain(line, "VALUE=DATE")) {
        return ICS_DT_ALLDAY;
    }
    size_t len = strlen(value);
    return (len > 0 && value[len - 1] == 'Z') ? ICS_DT_UTC : ICS_DT_LOCAL;
}

bool ics_client_fetch(esp_http_client_handle_t client, const app_calendar_cfg_t *cal,
                       uint8_t calendar_index, time_t time_min, time_t time_max,
                       gcal_event_t *out, int *inout_count, int max_out)
{
    ESP_LOGI(TAG, "[%s] fetching ICS feed", cal->label);

    /* MALLOC_CAP_SPIRAM - an ICS feed is a full, unwindowed calendar
     * export (no server-side date filtering the way the Google Calendar
     * API gets time_min/time_max query params), so this can grow much
     * larger than a single Google API page before gcal_client.c's shared
     * http_event_handler() (which this reuses - see the file comment
     * above) even gets a chance to realloc() it into PSRAM - a plain
     * malloc() here would sit on internal RAM, competing with mbedtls's
     * own internal-RAM-only TLS handshake buffers for the exact same
     * fetch's handshake. See the matching, more detailed comment on the
     * gcal_event_t buffer in gcal_client.c's gcal_refresh_all(). */
    struct http_resp_buf resp = {.data = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM), .len = 0, .cap = 4096};
    if (resp.data == NULL) {
        return false;
    }
    resp.data[0] = '\0';

    /* Config (buffer sizes, crt_bundle, event_handler) already set up by
     * gcal_refresh_all() when this shared client was created - just point
     * it at this feed's URL. */
    esp_http_client_set_url(client, cal->id);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_user_data(client, &resp);

    esp_err_t err = http_perform_with_retry(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] ICS request failed: %s", cal->label, esp_err_to_name(err));
        free(resp.data);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "[%s] ICS HTTP %d", cal->label, status);
        free(resp.data);
        return false;
    }

    normalize_and_unfold(resp.data);

    bool in_event = false;
    bool ev_rrule = false, ev_cancelled = false, ev_start_set = false, ev_end_set = false, ev_all_day = false;
    char ev_summary[GCAL_MAX_SUMMARY];
    time_t ev_start = 0, ev_end = 0;
    int added = 0, skipped_recurring = 0, skipped_incomplete = 0;

    char *line = resp.data;
    while (line != NULL) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
        }

        if (strncasecmp(line, "BEGIN:VEVENT", 12) == 0) {
            in_event = true;
            ev_rrule = ev_cancelled = ev_start_set = ev_end_set = ev_all_day = false;
            ev_summary[0] = '\0';
        } else if (strncasecmp(line, "END:VEVENT", 10) == 0) {
            if (in_event) {
                if (ev_rrule) {
                    skipped_recurring++;
                } else if (!ev_cancelled && ev_start_set && ev_end_set) {
                    if (*inout_count < max_out && ev_end > time_min && ev_start < time_max) {
                        gcal_event_t ev = {0};
                        strncpy(ev.summary, ev_summary[0] ? ev_summary : "(No title)", sizeof(ev.summary) - 1);
                        ev.start = ev_start;
                        ev.end = ev_end;
                        ev.all_day = ev_all_day;
                        ev.color = cal->color;
                        ev.calendar_index = calendar_index;
                        strncpy(ev.calendar_label, cal->label, sizeof(ev.calendar_label) - 1);
                        out[*inout_count] = ev;
                        (*inout_count)++;
                        added++;
                    }
                } else if (!ev_cancelled) {
                    skipped_incomplete++;
                }
            }
            in_event = false;
        } else if (in_event) {
            const char *val;
            if (prop_match(line, "SUMMARY", &val)) {
                ics_unescape_text(val, ev_summary, sizeof(ev_summary));
            } else if (prop_match(line, "STATUS", &val)) {
                if (strncasecmp(val, "CANCELLED", 9) == 0) {
                    ev_cancelled = true;
                }
            } else if (strncasecmp(line, "RRULE", 5) == 0 && (line[5] == ':' || line[5] == ';')) {
                ev_rrule = true;
            } else if (prop_match(line, "DTSTART", &val)) {
                if (parse_ics_datetime(val, dt_mode_for_line(line, val), &ev_start)) {
                    ev_start_set = true;
                    ev_all_day = params_contain(line, "VALUE=DATE");
                }
            } else if (prop_match(line, "DTEND", &val)) {
                if (parse_ics_datetime(val, dt_mode_for_line(line, val), &ev_end)) {
                    ev_end_set = true;
                }
            }
        }

        line = (next != NULL) ? next + 1 : NULL;
    }

    free(resp.data);

    if (skipped_recurring > 0) {
        ESP_LOGI(TAG, "[%s] skipped %d recurring event(s) - not expanded yet", cal->label, skipped_recurring);
    }
    if (skipped_incomplete > 0) {
        ESP_LOGI(TAG, "[%s] skipped %d event(s) missing DTSTART/DTEND", cal->label, skipped_incomplete);
    }
    ESP_LOGI(TAG, "[%s] %d event(s)", cal->label, added);
    return true;
}
