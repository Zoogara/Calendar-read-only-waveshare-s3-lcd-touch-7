#include "provisioning.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "prov_web";

static app_settings_t s_pending;

/* ---------------- tiny x-www-form-urlencoded helpers ---------------- */

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

/* Finds `key=` in a urlencoded body, url-decodes the value into `out`
 * (size out_sz), returns true if found (even if empty). */
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
            /* collapse CRLF -> LF in place */
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

/* ---------------- HTML ---------------- */

static const char *HTML_HEAD =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Calendar Display Setup</title><style>"
    "body{font-family:sans-serif;max-width:640px;margin:1.5em auto;padding:0 1em;color:#222}"
    "h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em;border-top:1px solid #ddd;padding-top:1em}"
    "label{display:block;margin-top:.8em;font-weight:600;font-size:.9em}"
    "input[type=text],input[type=password],input[type=number],textarea{width:100%;box-sizing:border-box;"
    "padding:.5em;font-size:1em;margin-top:.2em}"
    "textarea{font-family:monospace;font-size:.8em}"
    ".cal-row{display:flex;gap:.5em;align-items:center;margin-top:.6em}"
    ".cal-row input[type=text]{flex:1}"
    ".hint{color:#666;font-size:.82em;margin-top:.2em}"
    "button{margin-top:1.6em;padding:.7em 1.4em;font-size:1em;background:#1a73e8;color:#fff;border:0;border-radius:4px}"
    "</style></head><body>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Calendar Display setup</h1>"
        "<p>Fill this in once. See the README for how to get the Google "
        "service-account email/key and how to share your calendars with "
        "it.</p>"
        "<form method='POST' action='/save'>"
        "<h2>Wi-Fi</h2>"
        "<label>SSID</label><input type='text' name='wifi_ssid' required>"
        "<label>Password</label><input type='password' name='wifi_password'>"

        "<h2>Google service account</h2>"
        "<label>Service account email (client_email)</label>"
        "<input type='text' name='sa_client_email' placeholder='xxx@your-project.iam.gserviceaccount.com' required>"
        "<label>Private key (private_key field from the downloaded JSON key, "
        "including the BEGIN/END PRIVATE KEY lines)</label>"
        "<textarea name='sa_private_key_pem' rows='10' required "
        "placeholder='-----BEGIN PRIVATE KEY-----&#10;...&#10;-----END PRIVATE KEY-----'></textarea>"

        "<h2>Calendars</h2>"
        "<div class='hint'>\"Google\" calendars use their ID (your Gmail "
        "address for your primary calendar, or the \"Calendar ID\" from "
        "that calendar's settings page) and must be shared with the "
        "service account above. \"ICS URL\" calendars use a feed's "
        "\"Secret address in iCal format\" instead - for calendars that "
        "can't be shared with the service account at all; recurring "
        "events aren't shown yet for this source. Leave ID/URL blank to "
        "skip a row. Tick \"daily\" for a rarely-changing or flaky "
        "calendar to fetch it just once a day (and at startup) rather "
        "than every refresh cycle.</div>");

    for (int i = 0; i < APP_SETTINGS_MAX_CALENDARS; i++) {
        char row[850];
        snprintf(row, sizeof(row),
            "<div class='cal-row'>"
            "<select name='cal_source_%d' style='width:auto'>"
            "<option value='0'>Google</option><option value='1'>ICS URL</option></select>"
            "<input type='text' name='cal_id_%d' placeholder='calendar id / email / ICS URL'>"
            "<input type='text' name='cal_label_%d' placeholder='label' style='max-width:9em'>"
            "<input type='color' name='cal_color_%d' value='#4285F4'>"
            "<label style='margin:0;font-weight:400'><input type='checkbox' name='cal_enabled_%d' checked style='width:auto'> on</label>"
            "<label style='margin:0;font-weight:400'><input type='checkbox' name='cal_daily_%d' style='width:auto'> daily</label>"
            "</div>", i, i, i, i, i, i);
        httpd_resp_sendstr_chunk(req, row);
    }

    httpd_resp_sendstr_chunk(req,
        "<h2>Other</h2>"
        "<label>Timezone (POSIX TZ string)</label>"
        "<input type='text' name='posix_tz' value='AEST-10AEDT,M10.1.0,M4.1.0/3'>"
        "<div class='hint'>Default is Australia/Melbourne. Find yours by "
        "searching \"POSIX TZ string &lt;your city&gt;\".</div>"
        "<label>Refresh interval (seconds)</label>"
        "<input type='number' name='refresh_interval_s' value='300' min='60'>"
        "<label>Firmware update URL (optional)</label>"
        "<input type='text' name='ota_url' placeholder='https://.../gcal_display.bin'>"
        "<div class='hint'>HTTPS URL of a firmware .bin - leave blank if you "
        "don't use the on-device \"Check for update\" option.</div>"
        "<button type='submit'>Save &amp; restart</button>"
        "</form></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
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

    memset(&s_pending, 0, sizeof(s_pending));
    form_get(body, "wifi_ssid", s_pending.wifi_ssid, sizeof(s_pending.wifi_ssid));
    form_get(body, "wifi_password", s_pending.wifi_password, sizeof(s_pending.wifi_password));
    form_get(body, "sa_client_email", s_pending.sa_client_email, sizeof(s_pending.sa_client_email));
    form_get(body, "sa_private_key_pem", s_pending.sa_private_key_pem, sizeof(s_pending.sa_private_key_pem));
    form_get(body, "posix_tz", s_pending.posix_tz, sizeof(s_pending.posix_tz));
    form_get(body, "ota_url", s_pending.ota_url, sizeof(s_pending.ota_url));

    char num[16];
    s_pending.refresh_interval_s = APP_SETTINGS_DEFAULT_REFRESH_S;
    if (form_get(body, "refresh_interval_s", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v >= 60) {
            s_pending.refresh_interval_s = (uint32_t)v;
        }
    }

    /* Not on this form - only adjustable on-device (gear icon -> Display,
     * see ui_settings_dialog.c) - but must still be set explicitly rather
     * than left at the memset() zero above, since 0 is itself a valid
     * "never sleep" screen_timeout_s value, so a fresh setup would
     * otherwise silently disable the screensaver instead of getting the
     * intended 5-minute default. */
    s_pending.screen_timeout_s = APP_SETTINGS_DEFAULT_SCREEN_TIMEOUT_S;
    s_pending.view_start_hour = APP_SETTINGS_DEFAULT_VIEW_START_HOUR;
    s_pending.view_end_hour = APP_SETTINGS_DEFAULT_VIEW_END_HOUR;
    s_pending.fetch_past_days = APP_SETTINGS_DEFAULT_FETCH_PAST_DAYS;
    s_pending.fetch_future_days = APP_SETTINGS_DEFAULT_FETCH_FUTURE_DAYS;

    int n = 0;
    for (int i = 0; i < APP_SETTINGS_MAX_CALENDARS; i++) {
        char key[24], id[APP_SETTINGS_MAX_CAL_ID], label[40], color[16], enabled[8], source[4], daily[8];
        snprintf(key, sizeof(key), "cal_id_%d", i);
        if (!form_get(body, key, id, sizeof(id)) || id[0] == '\0') {
            continue;
        }
        app_calendar_cfg_t *c = &s_pending.calendars[n];
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
    s_pending.calendar_count = n;
    free(body);

    bool valid = s_pending.wifi_ssid[0] && s_pending.sa_client_email[0] &&
                 s_pending.sa_private_key_pem[0] && s_pending.calendar_count > 0;
    if (!valid) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><body><p>Missing required fields (SSID, service account "
            "email/key, or at least one calendar). "
            "<a href='/'>Go back</a></p></body></html>");
        return ESP_OK;
    }
    if (s_pending.posix_tz[0] == '\0') {
        strncpy(s_pending.posix_tz, "AEST-10AEDT,M10.1.0,M4.1.0/3", sizeof(s_pending.posix_tz) - 1);
    }

    esp_err_t err = provisioning_save(&s_pending);
    httpd_resp_set_type(req, "text/html");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req,
            "<html><body><p>Saved. Restarting into normal mode now - "
            "reconnect to your regular Wi-Fi network to see the "
            "display.</p></body></html>");
        ESP_LOGI(TAG, "provisioning complete, restarting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_sendstr(req, "<html><body><p>Save failed, try again.</p></body></html>");
    }
    return ESP_OK;
}

static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = PROVISIONING_AP_SSID,
            .ssid_len = strlen(PROVISIONING_AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = strlen(PROVISIONING_AP_PSK) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.password, PROVISIONING_AP_PSK, sizeof(ap_cfg.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "provisioning AP \"%s\" up - connect and browse to 192.168.4.1", PROVISIONING_AP_SSID);
}

void provisioning_run_portal(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    start_ap();

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 8192;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);

    /* Everything happens in HTTP handlers from here; just idle. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
