#include "config_web.h"
#include "provisioning.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "config_web";

static app_settings_t *s_cfg;
static httpd_handle_t s_server;

/* ---------------- tiny x-www-form-urlencoded helpers ----------------
 * Same small helpers as prov_web.c's setup portal - duplicated rather
 * than shared, since that file's are file-static and this server has a
 * different lifecycle (starts once Wi-Fi is up and runs indefinitely,
 * rather than running once before Wi-Fi exists at all) that isn't worth
 * entangling with the setup portal's. */

static void urldecode_inplace(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '+') {
            *w++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t keylen = strlen(key);
    const char *p = body;
    while (p != NULL) {
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *val_start = p + keylen + 1;
            const char *amp = strchr(val_start, '&');
            size_t vlen = amp ? (size_t)(amp - val_start) : strlen(val_start);
            if (vlen >= out_sz) {
                vlen = out_sz - 1;
            }
            memcpy(out, val_start, vlen);
            out[vlen] = '\0';
            urldecode_inplace(out);
            char *src = out, *dst = out;
            while (*src) {
                if (*src == '\r') {
                    src++;
                    continue;
                }
                *dst++ = *src++;
            }
            *dst = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

/* ---------------- auth ---------------- */

static void send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Calendar Display\"");
    httpd_resp_send(req, NULL, 0);
}

/* Only the password half of "user:pass" is checked - any (or no)
 * username is accepted, since this device has exactly one shared
 * password, not real per-user accounts. */
static bool check_auth(httpd_req_t *req)
{
    if (s_cfg->config_web_password[0] == '\0') {
        send_401(req);
        return false;
    }

    char auth_hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK ||
        strncmp(auth_hdr, "Basic ", 6) != 0) {
        send_401(req);
        return false;
    }

    unsigned char decoded[192];
    size_t decoded_len = 0;
    const char *b64 = auth_hdr + 6;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *)b64, strlen(b64)) != 0) {
        send_401(req);
        return false;
    }
    decoded[decoded_len] = '\0';

    /* user-pass = userid ":" password - password may itself contain ':',
     * username may not, so split on the *first* colon only. */
    char *colon = strchr((char *)decoded, ':');
    const char *password = colon ? colon + 1 : (char *)decoded;

    if (strcmp(password, s_cfg->config_web_password) != 0) {
        send_401(req);
        return false;
    }
    return true;
}

/* ---------------- HTML ---------------- */

static const char *HTML_HEAD =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Calendar Display Config</title><style>"
    "body{font-family:sans-serif;max-width:640px;margin:1.5em auto;padding:0 1em;color:#222}"
    "h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em;border-top:1px solid #ddd;padding-top:1em}"
    "label{display:block;margin-top:.8em;font-weight:600;font-size:.9em}"
    "input[type=text],input[type=number],textarea{width:100%;box-sizing:border-box;"
    "padding:.5em;font-size:1em;margin-top:.2em}"
    "textarea{font-family:monospace;font-size:.8em}"
    "input[type=range]{width:100%;margin-top:.4em}"
    ".range-val{color:#666;font-size:.85em}"
    ".cal-row{display:flex;gap:.5em;align-items:center;margin-top:.6em}"
    ".cal-row input[type=text]{flex:1}"
    ".hint{color:#666;font-size:.82em;margin-top:.2em}"
    ".btn-row{display:flex;gap:.8em;margin-top:1.6em}"
    "button,.btn-cancel{padding:.7em 1.4em;font-size:1em;border-radius:4px;text-decoration:none;"
    "display:inline-block;box-sizing:border-box}"
    "button{background:#1a73e8;color:#fff;border:0}"
    ".btn-cancel{background:#fff;color:#333;border:1px solid #ccc}"
    "</style></head><body>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Calendar Display config</h1>"
        "<p>Wi-Fi isn't changeable here - use the gear icon on the device "
        "(Setup) for that.</p>"
        "<form method='POST' action='/save'>"
        "<h2>Google service account</h2>"
        "<label>Service account email (client_email)</label>");
    httpd_resp_sendstr_chunk(req, "<input type='text' name='sa_client_email' value='");
    httpd_resp_sendstr_chunk(req, s_cfg->sa_client_email);
    httpd_resp_sendstr_chunk(req,
        "' required>"
        "<label>Private key (private_key field from the downloaded JSON key, "
        "including the BEGIN/END PRIVATE KEY lines)</label>"
        "<textarea name='sa_private_key_pem' rows='10' required>");
    httpd_resp_sendstr_chunk(req, s_cfg->sa_private_key_pem);
    httpd_resp_sendstr_chunk(req,
        "</textarea>"
        "<h2>Calendars</h2>"
        "<div class='hint'>\"Google\" calendars use their ID (your Gmail "
        "address for your primary calendar, or the \"Calendar ID\" from "
        "that calendar's settings page) and must be shared with the "
        "service account above. \"ICS URL\" calendars use a feed's "
        "\"Secret address in iCal format\" instead - for calendars that "
        "can't be shared with the service account at all; recurring "
        "events aren't shown yet for this source. Leave ID/URL blank to "
        "skip a row. Tick \"daily\" for a calendar that barely ever "
        "changes (a holidays feed) or a flaky host not worth hammering - "
        "it's then only fetched once a day (and at startup) instead of "
        "every refresh cycle, keeping its last-fetched events in between.</div>");

    for (int i = 0; i < APP_SETTINGS_MAX_CALENDARS; i++) {
        const app_calendar_cfg_t *c = (i < s_cfg->calendar_count) ? &s_cfg->calendars[i] : NULL;
        bool is_ics = c && c->source == APP_CAL_SOURCE_ICS;
        char row[APP_SETTINGS_MAX_CAL_ID + 1100];
        snprintf(row, sizeof(row),
            "<div class='cal-row'>"
            "<select name='cal_source_%d' style='width:auto'>"
            "<option value='0' %s>Google</option><option value='1' %s>ICS URL</option></select>"
            "<input type='text' name='cal_id_%d' placeholder='calendar id / email / ICS URL' value='%s'>"
            "<input type='text' name='cal_label_%d' placeholder='label' style='max-width:9em' value='%s'>"
            "<input type='color' name='cal_color_%d' value='#%06lX'>"
            "<label style='margin:0;font-weight:400'><input type='checkbox' name='cal_enabled_%d' %s style='width:auto'> on</label>"
            "<label style='margin:0;font-weight:400'><input type='checkbox' name='cal_daily_%d' %s style='width:auto'> daily</label>"
            "</div>", i, is_ics ? "" : "selected", is_ics ? "selected" : "",
            i, c ? c->id : "", i, c ? c->label : "", i,
            (unsigned long)(c ? c->color : 0x4285F4), i, (c == NULL || c->enabled) ? "checked" : "",
            i, (c && c->daily_only) ? "checked" : "");
        httpd_resp_sendstr_chunk(req, row);
    }

    char other[3600];
    snprintf(other, sizeof(other),
        "<h2>Other</h2>"
        "<label>Timezone (POSIX TZ string)</label>"
        "<input type='text' name='posix_tz' value='%s'>"
        "<div class='hint'>Find yours by searching \"POSIX TZ string &lt;your city&gt;\".</div>"
        "<label>Refresh interval (seconds)</label>"
        "<input type='number' name='refresh_interval_s' value='%u' min='60'>"
        "<label>Firmware update URL (optional)</label>"
        "<input type='text' name='ota_url' value='%s' placeholder='https://.../gcal_display.bin'>"
        "<label>Screen timeout (seconds, 0 = never sleep)</label>"
        "<input type='number' name='screen_timeout_s' value='%u' min='0'>"
        "<label>Week/day view hour range (24h clock)</label>"
        "<div class='cal-row'>"
        "<input type='number' name='view_start_hour' value='%u' min='0' max='23' style='flex:1'>"
        "<span>to</span>"
        "<input type='number' name='view_end_hour' value='%u' min='1' max='24' style='flex:1'>"
        "</div>"
        "<label>Calendar cache window (days around today)</label>"
        "<div class='cal-row'>"
        "<input type='number' name='fetch_past_days' value='%u' min='0' max='90' style='flex:1'>"
        "<span>past /</span>"
        "<input type='number' name='fetch_future_days' value='%u' min='1' max='365' style='flex:1'>"
        "<span>future</span>"
        "</div>"
        "<div class='hint'>How far back/forward each sync fetches and keeps "
        "cached - wider covers more paging without a fresh fetch, but costs "
        "more time/memory per sync. Dial back if syncs start struggling.</div>"
        "<div class='btn-row'>"
        "<button type='submit'>Save &amp; restart</button>"
        "<a class='btn-cancel' href='/'>Cancel</a>"
        "</div>"
        "</form></body></html>",
        s_cfg->posix_tz, (unsigned)s_cfg->refresh_interval_s, s_cfg->ota_url,
        (unsigned)s_cfg->screen_timeout_s, (unsigned)s_cfg->view_start_hour, (unsigned)s_cfg->view_end_hour,
        (unsigned)s_cfg->fetch_past_days, (unsigned)s_cfg->fetch_future_days);
    httpd_resp_sendstr_chunk(req, other);

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

struct save_task_arg {
    app_settings_t *cfg;
    SemaphoreHandle_t done;
    esp_err_t result;
};

/* provisioning_save() ends in nvs_commit(), an actual flash write - that
 * briefly disables the flash/PSRAM cache, which only the calling task's
 * OWN (internal-RAM) stack survives. This server's httpd worker task
 * deliberately keeps its stack in PSRAM (see config_web_start() below,
 * needed so the calendar-fetch task's mbedtls/TLS work isn't starved of
 * internal RAM while this always-on second server sits idle) - so the
 * save itself has to happen on a separate, short-lived task with a
 * normal internal-RAM stack instead of inline in the httpd handler.
 * Calling it directly from the httpd task crashed with
 * "esp_task_stack_is_sane_cache_disabled" mid-write, restarting the
 * device without ever finishing the save. */
static void save_task(void *arg)
{
    struct save_task_arg *a = (struct save_task_arg *)arg;
    a->result = provisioning_save(a->cfg);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }

    size_t total = req->content_len;
    if (total == 0 || total > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad content length");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[total] = '\0';

    /* Update s_cfg in place, not a fresh zeroed struct - wifi_ssid/
     * wifi_password (and config_web_password itself) aren't on this form
     * at all and must survive untouched. */
    form_get(body, "sa_client_email", s_cfg->sa_client_email, sizeof(s_cfg->sa_client_email));
    form_get(body, "sa_private_key_pem", s_cfg->sa_private_key_pem, sizeof(s_cfg->sa_private_key_pem));
    form_get(body, "posix_tz", s_cfg->posix_tz, sizeof(s_cfg->posix_tz));
    form_get(body, "ota_url", s_cfg->ota_url, sizeof(s_cfg->ota_url));

    char num[16];
    if (form_get(body, "refresh_interval_s", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 60) {
            s_cfg->refresh_interval_s = (uint32_t)v;
        }
    }
    if (form_get(body, "screen_timeout_s", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 0) {
            s_cfg->screen_timeout_s = (uint32_t)v;
        }
    }
    uint8_t new_start = s_cfg->view_start_hour, new_end = s_cfg->view_end_hour;
    if (form_get(body, "view_start_hour", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 0 && v <= 23) {
            new_start = (uint8_t)v;
        }
    }
    if (form_get(body, "view_end_hour", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 1 && v <= 24) {
            new_end = (uint8_t)v;
        }
    }
    if (new_start < new_end) {
        s_cfg->view_start_hour = new_start;
        s_cfg->view_end_hour = new_end;
    }
    if (form_get(body, "fetch_past_days", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 0 && v <= 90) {
            s_cfg->fetch_past_days = (uint16_t)v;
        }
    }
    if (form_get(body, "fetch_future_days", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 1 && v <= 365) {
            s_cfg->fetch_future_days = (uint16_t)v;
        }
    }

    app_calendar_cfg_t new_cals[APP_SETTINGS_MAX_CALENDARS];
    memset(new_cals, 0, sizeof(new_cals));
    int n = 0;
    for (int i = 0; i < APP_SETTINGS_MAX_CALENDARS; i++) {
        char key[24], id[APP_SETTINGS_MAX_CAL_ID], label[40], color[16], enabled[8], source[4], daily[8];
        snprintf(key, sizeof(key), "cal_id_%d", i);
        if (!form_get(body, key, id, sizeof(id)) || id[0] == '\0') {
            continue;
        }
        app_calendar_cfg_t *c = &new_cals[n];
        strncpy(c->id, id, sizeof(c->id) - 1);

        snprintf(key, sizeof(key), "cal_source_%d", i);
        c->source = (form_get(body, key, source, sizeof(source)) && atoi(source) == APP_CAL_SOURCE_ICS)
                        ? APP_CAL_SOURCE_ICS : APP_CAL_SOURCE_GOOGLE;

        snprintf(key, sizeof(key), "cal_label_%d", i);
        if (form_get(body, key, label, sizeof(label)) && label[0] != '\0') {
            strncpy(c->label, label, sizeof(c->label) - 1);
        } else {
            strncpy(c->label, id, sizeof(c->label) - 1);
        }

        snprintf(key, sizeof(key), "cal_color_%d", i);
        c->color = 0x808080;
        if (form_get(body, key, color, sizeof(color)) && color[0] == '#') {
            c->color = (uint32_t)strtol(color + 1, NULL, 16);
        }

        snprintf(key, sizeof(key), "cal_enabled_%d", i);
        c->enabled = form_get(body, key, enabled, sizeof(enabled));

        snprintf(key, sizeof(key), "cal_daily_%d", i);
        c->daily_only = form_get(body, key, daily, sizeof(daily));
        n++;
    }
    free(body);

    bool valid = s_cfg->sa_client_email[0] && s_cfg->sa_private_key_pem[0] && n > 0;
    if (!valid) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><body><p>Missing required fields (service account "
            "email/key, or at least one calendar). "
            "<a href='/'>Go back</a></p></body></html>");
        return ESP_OK;
    }

    memcpy(s_cfg->calendars, new_cals, sizeof(new_cals));
    s_cfg->calendar_count = n;

    struct save_task_arg arg = {.cfg = s_cfg, .done = xSemaphoreCreateBinary(), .result = ESP_FAIL};
    xTaskCreate(save_task, "config_save", 4096, &arg, 5, NULL);
    xSemaphoreTake(arg.done, portMAX_DELAY);
    vSemaphoreDelete(arg.done);
    esp_err_t err = arg.result;

    httpd_resp_set_type(req, "text/html");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "<html><body><p>Saved. Restarting now.</p></body></html>");
        ESP_LOGI(TAG, "config saved via web, restarting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_sendstr(req, "<html><body><p>Save failed, try again.</p></body></html>");
    }
    return ESP_OK;
}

void config_web_start(app_settings_t *cfg)
{
    if (s_server != NULL) {
        return;
    }
    if (cfg->config_web_password[0] == '\0') {
        ESP_LOGI(TAG, "no config web password set - not starting (set one from the "
                      "gear icon -> Display Settings to enable)");
        return;
    }
    s_cfg = cfg;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 8192;
    /* This always-on second HTTP server task sits idle almost all the
     * time, so keep its stack off internal RAM - it otherwise competes
     * with the calendar-fetch task's mbedtls/TLS work for the same small
     * pool (see gcal_client.c's log_heap_state() comment for how that
     * fragmentation broke certificate verification once before, with
     * mDNS as the other tenant). The one place that would be unsafe -
     * the /save handler's actual flash write - is deliberately NOT run on
     * this task; see save_task() above for why and how it's kept on a
     * separate internal-RAM task instead. */
    http_cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    if (httpd_start(&s_server, &http_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "failed to start config web server");
        s_server = NULL;
        return;
    }

    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &save_uri);

    ESP_LOGI(TAG, "config web server up");
}
