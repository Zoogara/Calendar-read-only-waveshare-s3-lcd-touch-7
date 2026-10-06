/* Weather details for the panel behind the clock's temperature (see
 * calendar_ui/ui_weather.c): current readings from the configured sensors
 * plus a 7-day forecast from the Bureau of Meteorology integration's
 * entities, all in ONE request - a POST to Home Assistant's /api/template
 * with a Jinja template that renders every value as JSON. Fetching each of
 * the ~50 entities separately would be ~50 requests per refresh.
 *
 * Every value goes through Jinja's |tojson, so quotes or odd characters in
 * the forecast text can't break the JSON. The rendered reply is ~3KB; it
 * and the parsed result live in PSRAM. */
#include "ha_weather.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ha_weather";

#define HTTP_TIMEOUT_MS   8000
#define STALE_AFTER_US    (45LL * 60 * 1000000)
#define TEMPLATE_MAX      4096

static SemaphoreHandle_t s_lock;
static ha_weather_details_t *s_details;   /* PSRAM; NULL until the first success */
static int64_t s_ok_us = -1;

struct resp_buf {
    char *data;
    size_t len, cap;
};

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    struct resp_buf *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + evt->data_len + 1 > b->cap) {
            size_t cap = (b->len + evt->data_len + 1) * 2;
            char *g = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM);
            if (g == NULL) {
                return ESP_FAIL;
            }
            b->data = g;
            b->cap = cap;
        }
        memcpy(b->data + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->data[b->len] = '\0';
    }
    return ESP_OK;
}

/* Appends to the template being built; false once it's full. */
static bool append(char *t, size_t *len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool append(char *t, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t + *len, TEMPLATE_MAX - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || *len + n >= TEMPLATE_MAX) {
        return false;
    }
    *len += n;
    return true;
}

/* "key":[state, unit] for one sensor - skipped when it isn't configured. */
static bool append_reading(char *t, size_t *len, const char *key, const char *entity)
{
    if (entity[0] == '\0') {
        return true;
    }
    return append(t, len, "\"%s\":{{ [states('%s'), state_attr('%s','unit_of_measurement')]|tojson }},",
                  key, entity, entity);
}

static bool build_template(const app_settings_t *cfg, char *t)
{
    size_t len = 0;
    bool ok = append(t, &len, "{\"obs\":{") &&
              append_reading(t, &len, "feels", cfg->ha_feels_entity) &&
              append_reading(t, &len, "wind", cfg->ha_wind_entity) &&
              append_reading(t, &len, "gust", cfg->ha_gust_entity) &&
              append_reading(t, &len, "wdir", cfg->ha_wdir_entity) &&
              append_reading(t, &len, "rain", cfg->ha_rain_entity) &&
              append_reading(t, &len, "pressure", cfg->ha_pressure_entity) &&
              append(t, &len, "\"_\":0}");
    if (ok && cfg->ha_bom_prefix[0] != '\0') {
        ok = append(t, &len,
            ",\"days\":[{%% set p = '%s' %%}{%% for i in range(%d) %%}"
            "{\"s\":{{ states(p~'short_text_'~i)|tojson }},"
            "\"i\":{{ states(p~'icon_descriptor_'~i)|tojson }},"
            "\"lo\":{{ states(p~'temp_min_'~i)|tojson }},"
            "\"hi\":{{ states(p~'temp_max_'~i)|tojson }},"
            "\"rc\":{{ states(p~'rain_chance_'~i)|tojson }},"
            "\"rr\":{{ states(p~'rain_amount_range_'~i)|tojson }},"
            "\"x\":{{ (states(p~'extended_text_'~i) if i < 2 else '')|tojson }}}"
            "{{ ',' if not loop.last else '' }}{%% endfor %%}]",
            cfg->ha_bom_prefix, HA_FORECAST_DAYS);
    }
    return ok && append(t, &len, "}");
}

static bool usable(const char *s)
{
    return s[0] != '\0' && strcasecmp(s, "unknown") != 0 && strcasecmp(s, "unavailable") != 0 &&
           strcasecmp(s, "none") != 0;
}

static void copy_str(char *dst, size_t sz, cJSON *j)
{
    dst[0] = '\0';
    if (!cJSON_IsString(j) || !usable(j->valuestring)) {
        return;
    }
    /* Truncate on a UTF-8 character boundary. */
    size_t n = strlen(j->valuestring);
    if (n >= sz) {
        n = sz - 1;
        while (n > 0 && ((unsigned char)j->valuestring[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, j->valuestring, n);
    dst[n] = '\0';
}

static void read_reading(ha_reading_t *r, cJSON *obs, const char *key)
{
    cJSON *pair = cJSON_GetObjectItemCaseSensitive(obs, key);
    copy_str(r->value, sizeof(r->value), cJSON_GetArrayItem(pair, 0));
    copy_str(r->unit, sizeof(r->unit), cJSON_GetArrayItem(pair, 1));
}

static esp_err_t fetch(const app_settings_t *cfg, ha_weather_details_t *out)
{
    char url[160];
    const char *base = cfg->ha_base_url;
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
    bool has_scheme = strncasecmp(base, "http://", 7) == 0;
    if (strncasecmp(base, "https://", 8) == 0) {
        return ESP_ERR_NOT_SUPPORTED;   /* see ha_weather.c */
    }
    snprintf(url, sizeof(url), "%s%.*s/api/template", has_scheme ? "" : "http://", (int)base_len, base);

    char *tmpl = heap_caps_malloc(TEMPLATE_MAX, MALLOC_CAP_SPIRAM);
    if (tmpl == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!build_template(cfg, tmpl)) {
        free(tmpl);
        ESP_LOGW(TAG, "details: template too long");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "template", tmpl);
    free(tmpl);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    struct resp_buf resp = { .data = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM), .cap = 4096 };
    if (resp.data == NULL) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';

    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_data,
        .user_data = &resp,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        cJSON_free(body);
        free(resp.data);
        return ESP_ERR_NO_MEM;
    }
    char auth[APP_SETTINGS_MAX_HA_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->ha_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(auth, 0, sizeof(auth));
    cJSON_free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "details: %s, HTTP %d%s (%s) %.120s", esp_err_to_name(err), status,
                 status == 401 ? " (token rejected)" : "", url, resp.data);
        free(resp.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    size_t bytes = resp.len;
    cJSON *root = cJSON_Parse(resp.data);
    free(resp.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "details: reply wasn't valid JSON (%u bytes)", (unsigned)bytes);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    cJSON *obs = cJSON_GetObjectItemCaseSensitive(root, "obs");
    read_reading(&out->feels, obs, "feels");
    read_reading(&out->wind, obs, "wind");
    read_reading(&out->gust, obs, "gust");
    read_reading(&out->wdir, obs, "wdir");
    read_reading(&out->rain, obs, "rain");
    read_reading(&out->pressure, obs, "pressure");

    cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    for (int i = 0; i < HA_FORECAST_DAYS; i++) {
        cJSON *d = cJSON_GetArrayItem(days, i);
        ha_forecast_day_t *f = &out->days[i];
        copy_str(f->short_text, sizeof(f->short_text), cJSON_GetObjectItemCaseSensitive(d, "s"));
        copy_str(f->icon, sizeof(f->icon), cJSON_GetObjectItemCaseSensitive(d, "i"));
        copy_str(f->lo, sizeof(f->lo), cJSON_GetObjectItemCaseSensitive(d, "lo"));
        copy_str(f->hi, sizeof(f->hi), cJSON_GetObjectItemCaseSensitive(d, "hi"));
        copy_str(f->rain_chance, sizeof(f->rain_chance), cJSON_GetObjectItemCaseSensitive(d, "rc"));
        copy_str(f->rain_range, sizeof(f->rain_range), cJSON_GetObjectItemCaseSensitive(d, "rr"));
        copy_str(f->extended, sizeof(f->extended), cJSON_GetObjectItemCaseSensitive(d, "x"));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "details: %u bytes; wind %s %s %s, forecast 0: %s/%s %s", (unsigned)bytes,
             out->wdir.value, out->wind.value, out->wind.unit, out->days[0].lo, out->days[0].hi,
             out->days[0].icon);
    return ESP_OK;
}

esp_err_t ha_weather_details_refresh(const app_settings_t *cfg)
{
    if (cfg->ha_base_url[0] == '\0' || cfg->ha_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    ha_weather_details_t *fresh = heap_caps_malloc(sizeof(*fresh), MALLOC_CAP_SPIRAM);
    if (fresh == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = fetch(cfg, fresh);
    if (err != ESP_OK) {
        free(fresh);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ha_weather_details_t *old = s_details;
    s_details = fresh;
    s_ok_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    free(old);   /* readers hold the lock while using it */
    return ESP_OK;
}

const ha_weather_details_t *ha_weather_details_acquire(void)
{
    if (s_lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_details == NULL || esp_timer_get_time() - s_ok_us > STALE_AFTER_US) {
        return NULL;
    }
    return s_details;
}

void ha_weather_details_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}
