#include "ha_weather.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ha_weather";

#define HTTP_TIMEOUT_MS      5000
#define RESPONSE_BUF_SZ      3072          /* one entity's JSON incl. attributes */
#define MIN_ATTEMPT_GAP_US   (60LL * 1000000)
#define STALE_AFTER_US       (45LL * 60 * 1000000)

/* Logs internal RAM before and after each refresh - the budget calendar
 * sync's TLS depends on (see gcal_client.c's log_heap_state()). DEBUG
 * level: an hour on real hardware (2026-10-04) showed no leak. */
#define HEAP_LOG(ctx) ESP_LOGD(TAG, "%s - internal: %u free / %u largest block", (ctx), \
                               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
                               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))

/* A spinlock is fine here: it only ever guards copying this ~40-byte
 * struct and a timestamp, with no blocking calls inside. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ha_weather_t s_value;
static int64_t s_last_ok_us = -1;       /* -1: never succeeded */
static int64_t s_last_attempt_us = -1;

/* GETs <base>/api/states/<entity> and copies the JSON "state" string into
 * state_out. */
static esp_err_t fetch_entity_state(const app_settings_t *cfg, const char *entity,
                                    char *state_out, size_t state_sz)
{
    char url[200];
    const char *base = cfg->ha_base_url;
    /* Plain http:// only: an https:// URL would add a TLS handshake to every
     * refresh, on top of the internal-RAM budget calendar sync already
     * strains. Home Assistant is on the same LAN. */
    if (strncasecmp(base, "https://", 8) == 0) {
        ESP_LOGW(TAG, "https:// isn't supported - use plain http:// on the LAN");
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool has_scheme = (strncasecmp(base, "http://", 7) == 0);
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
    int n = snprintf(url, sizeof(url), "%s%.*s/api/states/%s",
                     has_scheme ? "" : "http://", (int)base_len, base, entity);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
        .buffer_size = 1024,
        .buffer_size_tx = 768,   /* the Authorization header alone is ~200 bytes */
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* PSRAM: internal RAM is the scarce resource here. */
    char *buf = heap_caps_malloc(RESPONSE_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(RESPONSE_BUF_SZ);
    }
    if (buf == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    char auth[APP_SETTINGS_MAX_HA_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->ha_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    int total = 0;
    int status = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;   /* timed out or connection dropped before the headers */
        esp_http_client_close(client);
    } else if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        /* esp_http_client_read() decodes chunked transfer-encoding itself
         * and returns 0 at the end of the body, so this loop works whether
         * or not HA sends a Content-Length. */
        for (;;) {
            int r = esp_http_client_read(client, buf + total, RESPONSE_BUF_SZ - 1 - total);
            if (r <= 0) {
                break;
            }
            total += r;
            if (total >= RESPONSE_BUF_SZ - 1) {
                break;
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    memset(auth, 0, sizeof(auth)); /* don't leave the token lying on the stack */

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: request failed: %s", entity, esp_err_to_name(err));
        free(buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d%s", entity, status,
                 status == 401 ? " (token rejected)" : status == 404 ? " (no such entity)" : "");
        free(buf);
        return ESP_FAIL;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s: response wasn't valid JSON (truncated?)", entity);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(st) || st->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strncpy(state_out, st->valuestring, state_sz - 1);
    state_out[state_sz - 1] = '\0';
    cJSON_Delete(root);
    return ESP_OK;
}

static bool is_unusable_state(const char *s)
{
    return s[0] == '\0' || strcasecmp(s, "unknown") == 0 ||
           strcasecmp(s, "unavailable") == 0 || strcasecmp(s, "none") == 0;
}

/* "partly_cloudy" -> "Partly cloudy" */
static void prettify_description(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_sz; i++) {
        unsigned char c = (in[i] == '_') ? ' ' : (unsigned char)in[i];
        out[o] = (o == 0) ? (char)toupper(c) : (char)tolower(c);
        o++;
    }
    out[o] = '\0';
}

static esp_err_t refresh_now(const app_settings_t *cfg);

esp_err_t ha_weather_refresh(const app_settings_t *cfg)
{
    if (cfg->ha_base_url[0] == '\0' || cfg->ha_token[0] == '\0' || cfg->ha_temp_entity[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now = esp_timer_get_time();
    if (s_last_attempt_us >= 0 && now - s_last_attempt_us < MIN_ATTEMPT_GAP_US) {
        return ESP_OK;
    }
    s_last_attempt_us = now;

    HEAP_LOG("refresh start");
    esp_err_t err = refresh_now(cfg);
    HEAP_LOG("refresh end");
    return err;
}

static esp_err_t refresh_now(const app_settings_t *cfg)
{
    char state[48];
    esp_err_t err = fetch_entity_state(cfg, cfg->ha_temp_entity, state, sizeof(state));
    if (err != ESP_OK) {
        return err;
    }
    char *end = NULL;
    float temp = strtof(state, &end);
    if (end == state || is_unusable_state(state) || temp < -60.0f || temp > 70.0f) {
        ESP_LOGW(TAG, "%s: unusable temperature state \"%s\"", cfg->ha_temp_entity, state);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ha_weather_t fresh = { .temp_c = temp, .night = -1 };
    if (cfg->ha_desc_entity[0] != '\0') {
        if (fetch_entity_state(cfg, cfg->ha_desc_entity, state, sizeof(state)) == ESP_OK &&
            !is_unusable_state(state)) {
            prettify_description(state, fresh.desc, sizeof(fresh.desc));
            size_t i = 0;
            for (; state[i] != '\0' && i + 1 < sizeof(fresh.cond); i++) {
                fresh.cond[i] = (char)tolower((unsigned char)state[i]);
            }
            fresh.cond[i] = '\0';

            /* Day or night icon - same test as "sun elevation <= 0". Only
             * worth a request when there's a condition to draw. */
            if (fetch_entity_state(cfg, "sun.sun", state, sizeof(state)) == ESP_OK) {
                if (strcmp(state, "below_horizon") == 0) {
                    fresh.night = 1;
                } else if (strcmp(state, "above_horizon") == 0) {
                    fresh.night = 0;
                }
            }
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_value = fresh;
    s_last_ok_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "%.1f C, \"%s\" (%s, %s)", fresh.temp_c, fresh.desc, fresh.cond,
             fresh.night < 0 ? "sun unknown" : fresh.night ? "night" : "day");
    return ESP_OK;
}

bool ha_weather_get(ha_weather_t *out)
{
    bool ok;
    taskENTER_CRITICAL(&s_lock);
    ok = (s_last_ok_us >= 0) && (esp_timer_get_time() - s_last_ok_us < STALE_AFTER_US);
    if (ok) {
        *out = s_value;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}
