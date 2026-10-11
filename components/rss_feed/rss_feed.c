#include "rss_feed.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rss_feed";

/* Some news CDNs turn away clients that don't look like a browser. */
#define USER_AGENT      "Mozilla/5.0 (compatible; gcal-display/1.0; +https://github.com/Zoogara/Calendar-read-only-waveshare-s3-lcd-touch-7)"
#define HTTP_TIMEOUT_MS 20000
#define MAX_BYTES       (160 * 1024)
#define REFRESH_US      (20LL * 60 * 1000000)
#define RETRY_US        (5LL * 60 * 1000000)

static SemaphoreHandle_t s_lock;
static struct {
    rss_feed_t *cur;        /* PSRAM; NULL until the first success */
    int64_t next_try_us;    /* esp_timer time before which it isn't due */
    char url[sizeof(((app_settings_t *)0)->feed_url[0])];   /* what cur came from */
} s_feed[RSS_FEED_COUNT];

struct resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

/* Collects the body into PSRAM, ignoring anything past MAX_BYTES. */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    struct resp_buf *buf = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf->len < MAX_BYTES) {
        size_t take = evt->data_len;
        if (buf->len + take > MAX_BYTES) {
            take = MAX_BYTES - buf->len;
        }
        if (buf->len + take + 1 > buf->cap) {
            size_t new_cap = (buf->len + take + 1) * 2;
            if (new_cap > MAX_BYTES + 1) {
                new_cap = MAX_BYTES + 1;
            }
            char *grown = heap_caps_realloc(buf->data, new_cap, MALLOC_CAP_SPIRAM);
            if (grown == NULL) {
                return ESP_FAIL;
            }
            buf->data = grown;
            buf->cap = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, take);
        buf->len += take;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch(const char *url, rss_feed_t *out)
{
    struct resp_buf resp = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/rss+xml, application/atom+xml, application/xml, text/xml, */*");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || resp.data == NULL) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", url, esp_err_to_name(err), status);
        free(resp.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    char *scratch = heap_caps_malloc(RSS_SCRATCH_LEN, MALLOC_CAP_SPIRAM);
    if (scratch == NULL) {
        free(resp.data);
        return ESP_ERR_NO_MEM;
    }
    int n = rss_parse(resp.data, resp.len, out, scratch);
    ESP_LOGI(TAG, "%s: %d items from %u bytes (\"%s\")", url, n, (unsigned)resp.len, out->name);
    free(scratch);
    free(resp.data);
    return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t rss_feed_refresh(const app_settings_t *cfg)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    esp_err_t result = ESP_OK;
    for (int i = 0; i < RSS_FEED_COUNT; i++) {
        const char *url = cfg->feed_url[i];
        if (url[0] == '\0') {
            continue;
        }
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            continue;   /* not a web address - nothing to try */
        }
        int64_t t = esp_timer_get_time();
        if (s_feed[i].cur != NULL && strcmp(s_feed[i].url, url) != 0) {
            s_feed[i].next_try_us = 0;   /* the URL changed - refetch now */
        }
        if (t < s_feed[i].next_try_us) {
            continue;
        }

        rss_feed_t *fresh = heap_caps_malloc(sizeof(rss_feed_t), MALLOC_CAP_SPIRAM);
        if (fresh == NULL) {
            result = ESP_ERR_NO_MEM;
            continue;
        }
        esp_err_t err = fetch(url, fresh);
        if (err != ESP_OK) {
            free(fresh);
            s_feed[i].next_try_us = esp_timer_get_time() + RETRY_US;
            result = err;
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rss_feed_t *old = s_feed[i].cur;
        s_feed[i].cur = fresh;
        strncpy(s_feed[i].url, url, sizeof(s_feed[i].url) - 1);
        xSemaphoreGive(s_lock);
        free(old);   /* no reader can hold it: they take the lock first */
        s_feed[i].next_try_us = esp_timer_get_time() + REFRESH_US;
    }
    return result;
}

const rss_feed_t *rss_feed_acquire(int idx)
{
    if (s_lock == NULL) {
        return NULL;   /* never refreshed; nothing to lock */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return (idx >= 0 && idx < RSS_FEED_COUNT) ? s_feed[idx].cur : NULL;
}

void rss_feed_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}
