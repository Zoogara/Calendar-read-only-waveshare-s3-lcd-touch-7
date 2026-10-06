#include "onthisday.h"

#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "onthisday";

#define URL_FMT          "https://en.wikipedia.org/api/rest_v1/feed/onthisday/selected/%02d/%02d"
/* Wikimedia asks API clients to identify themselves with a User-Agent that
 * gives a way to reach the operator - the project's page, not a personal
 * address. */
#define USER_AGENT       "gcal-display/1.0 (https://github.com/Zoogara/Calendar-read-only-waveshare-s3-lcd-touch-7)"
#define HTTP_TIMEOUT_MS  15000
#define RETRY_GAP_US     (30LL * 60 * 1000000)

static SemaphoreHandle_t s_lock;
static onthisday_t *s_current;          /* PSRAM; NULL until the first success */
static int64_t s_last_attempt_us = -1;

struct resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    struct resp_buf *buf = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf->len + evt->data_len + 1 > buf->cap) {
            size_t new_cap = (buf->len + evt->data_len + 1) * 2;
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

/* Copies a UTF-8 string, truncating at a character boundary rather than
 * through the middle of a multi-byte sequence. */
static void copy_utf8(char *dst, const char *src, size_t dst_sz)
{
    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;   /* src[n] is a continuation byte - back up to the lead byte */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Wikipedia's text occasionally carries raw page markup - e.g. a tooltip's
 * ".mw-parser-output .tooltip-dotted{border-bottom:1px dotted;...}" glued to
 * the front of a unit - and a few characters no font here has: swaps the
 * no-break space variants for a plain space and the non-breaking hyphen for
 * a plain one, and drops any ".mw-parser-output ...{...}" block. */
static void clean_text(char *s)
{
    char *css;
    while ((css = strstr(s, ".mw-parser-output")) != NULL) {
        char *end = strchr(css, '}');
        if (end == NULL) {
            *css = '\0';
            break;
        }
        memmove(css, end + 1, strlen(end + 1) + 1);
    }
    char *w = s;
    for (const char *r = s; *r; ) {
        const unsigned char *u = (const unsigned char *)r;
        if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0xAF)) {
            *w++ = ' ';       /* U+202F narrow no-break space */
            r += 3;
        } else if (u[0] == 0xE2 && u[1] == 0x80 && u[2] == 0x91) {
            *w++ = '-';       /* U+2011 non-breaking hyphen */
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Wikipedia's text refers to its own images - "(pictured)", "(pictured
 * left)", "(example pictured)" - which mean nothing here. Removes any
 * parenthetical containing "pictured", plus the space before it. */
static void strip_pictured(char *s)
{
    char *p;
    while ((p = strstr(s, "pictured")) != NULL) {
        char *open = p;
        while (open > s && *open != '(') {
            open--;
        }
        char *close = strchr(p, ')');
        if (*open != '(' || close == NULL) {
            return;
        }
        if (open > s && open[-1] == ' ') {
            open--;
        }
        memmove(open, close + 1, strlen(close + 1) + 1);
    }
}

static esp_err_t fetch(int month, int day, onthisday_t *out)
{
    char url[128];
    snprintf(url, sizeof(url), URL_FMT, month, day);

    struct resp_buf resp = { .data = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM), .len = 0, .cap = 8192 };
    if (resp.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';

    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = USER_AGENT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        free(resp.data);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "request failed: %s, HTTP %d", esp_err_to_name(err), status);
        free(resp.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    size_t bytes = resp.len;
    cJSON *root = cJSON_Parse(resp.data);   /* PSRAM - see main.c's cJSON hooks */
    free(resp.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad JSON (%u bytes)", (unsigned)bytes);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    out->month = month;
    out->day = day;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "selected");
    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (out->count >= ONTHISDAY_MAX_ENTRIES) {
            break;
        }
        cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        cJSON *year = cJSON_GetObjectItemCaseSensitive(item, "year");
        if (!cJSON_IsString(text) || !cJSON_IsNumber(year)) {
            continue;
        }
        onthisday_entry_t *e = &out->entries[out->count];
        e->year = (int16_t)year->valueint;
        copy_utf8(e->text, text->valuestring, sizeof(e->text));
        clean_text(e->text);
        strip_pictured(e->text);

        /* Heading: the first linked article - the entry's main subject. */
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(item, "pages");
        cJSON *page = cJSON_IsArray(pages) ? cJSON_GetArrayItem(pages, 0) : NULL;
        cJSON *titles = page ? cJSON_GetObjectItemCaseSensitive(page, "titles") : NULL;
        cJSON *norm = titles ? cJSON_GetObjectItemCaseSensitive(titles, "normalized") : NULL;
        if (cJSON_IsString(norm)) {
            copy_utf8(e->title, norm->valuestring, sizeof(e->title));
        }
        out->count++;
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "%d/%d: %d entries from %u bytes", day, month, out->count, (unsigned)bytes);
    return out->count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t onthisday_refresh(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year < 120) {
        return ESP_ERR_INVALID_STATE;   /* clock not set yet */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have_today = s_current && s_current->month == lt.tm_mon + 1 && s_current->day == lt.tm_mday;
    xSemaphoreGive(s_lock);
    if (have_today) {
        return ESP_OK;
    }
    int64_t t = esp_timer_get_time();
    if (s_last_attempt_us >= 0 && t - s_last_attempt_us < RETRY_GAP_US) {
        return ESP_OK;   /* failed recently - try again later */
    }
    s_last_attempt_us = t;

    onthisday_t *fresh = heap_caps_malloc(sizeof(onthisday_t), MALLOC_CAP_SPIRAM);
    if (fresh == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = fetch(lt.tm_mon + 1, lt.tm_mday, fresh);
    if (err != ESP_OK) {
        free(fresh);
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    onthisday_t *old = s_current;
    s_current = fresh;
    xSemaphoreGive(s_lock);
    free(old);   /* no reader can hold it: they take the lock first */
    s_last_attempt_us = -1;
    return ESP_OK;
}

const onthisday_t *onthisday_acquire(void)
{
    if (s_lock == NULL) {
        return NULL;   /* never refreshed; nothing to lock */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return s_current;
}

void onthisday_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}
