#include "provisioning.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "prov_store";
#define NVS_NAMESPACE "gcalcfg"
#define NVS_KEY_JSON  "json"

#define SD_CONFIG_FILE PROVISIONING_SD_DIR "/config.json"

static cJSON *settings_to_json(const app_settings_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", cfg->wifi_password);
    cJSON_AddStringToObject(root, "sa_client_email", cfg->sa_client_email);
    cJSON_AddStringToObject(root, "sa_private_key_pem", cfg->sa_private_key_pem);
    cJSON_AddStringToObject(root, "posix_tz", cfg->posix_tz);
    cJSON_AddNumberToObject(root, "refresh_interval_s", cfg->refresh_interval_s);
    cJSON_AddStringToObject(root, "ota_url", cfg->ota_url);
    cJSON_AddNumberToObject(root, "screen_timeout_s", cfg->screen_timeout_s);
    cJSON_AddNumberToObject(root, "view_start_hour", cfg->view_start_hour);
    cJSON_AddNumberToObject(root, "view_end_hour", cfg->view_end_hour);
    cJSON_AddNumberToObject(root, "fetch_past_days", cfg->fetch_past_days);
    cJSON_AddNumberToObject(root, "fetch_future_days", cfg->fetch_future_days);
    cJSON_AddStringToObject(root, "config_web_password", cfg->config_web_password);

    cJSON *cals = cJSON_AddArrayToObject(root, "calendars");
    for (int i = 0; i < cfg->calendar_count; i++) {
        const app_calendar_cfg_t *c = &cfg->calendars[i];
        cJSON *jc = cJSON_CreateObject();
        cJSON_AddNumberToObject(jc, "source", (double)c->source);
        cJSON_AddStringToObject(jc, "id", c->id);
        cJSON_AddStringToObject(jc, "label", c->label);
        cJSON_AddNumberToObject(jc, "color", (double)c->color);
        cJSON_AddBoolToObject(jc, "enabled", c->enabled);
        cJSON_AddItemToArray(cals, jc);
    }
    return root;
}

static void json_to_settings(cJSON *root, app_settings_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *j;
#define COPY_STR(field, key) \
    j = cJSON_GetObjectItemCaseSensitive(root, key); \
    if (cJSON_IsString(j) && j->valuestring) { \
        strncpy(out->field, j->valuestring, sizeof(out->field) - 1); \
    }
    COPY_STR(wifi_ssid, "wifi_ssid");
    COPY_STR(wifi_password, "wifi_password");
    COPY_STR(sa_client_email, "sa_client_email");
    COPY_STR(sa_private_key_pem, "sa_private_key_pem");
    COPY_STR(posix_tz, "posix_tz");
    COPY_STR(ota_url, "ota_url");
    COPY_STR(config_web_password, "config_web_password");
#undef COPY_STR

    j = cJSON_GetObjectItemCaseSensitive(root, "refresh_interval_s");
    out->refresh_interval_s = cJSON_IsNumber(j) && j->valuedouble > 0
                                   ? (uint32_t)j->valuedouble
                                   : APP_SETTINGS_DEFAULT_REFRESH_S;

    /* screen_timeout_s: unlike the fields above, 0 is a legitimate
     * configured value ("never sleep"), not something to fall back from -
     * only missing entirely (an NVS blob saved before this field existed)
     * should default. */
    j = cJSON_GetObjectItemCaseSensitive(root, "screen_timeout_s");
    out->screen_timeout_s = cJSON_IsNumber(j) ? (uint32_t)j->valuedouble : APP_SETTINGS_DEFAULT_SCREEN_TIMEOUT_S;

    j = cJSON_GetObjectItemCaseSensitive(root, "view_start_hour");
    out->view_start_hour = (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 23)
                                ? (uint8_t)j->valuedouble
                                : APP_SETTINGS_DEFAULT_VIEW_START_HOUR;

    j = cJSON_GetObjectItemCaseSensitive(root, "view_end_hour");
    out->view_end_hour = (cJSON_IsNumber(j) && j->valuedouble >= 1 && j->valuedouble <= 24)
                              ? (uint8_t)j->valuedouble
                              : APP_SETTINGS_DEFAULT_VIEW_END_HOUR;

    j = cJSON_GetObjectItemCaseSensitive(root, "fetch_past_days");
    out->fetch_past_days = (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 90)
                                ? (uint16_t)j->valuedouble
                                : APP_SETTINGS_DEFAULT_FETCH_PAST_DAYS;

    j = cJSON_GetObjectItemCaseSensitive(root, "fetch_future_days");
    out->fetch_future_days = (cJSON_IsNumber(j) && j->valuedouble >= 1 && j->valuedouble <= 365)
                                  ? (uint16_t)j->valuedouble
                                  : APP_SETTINGS_DEFAULT_FETCH_FUTURE_DAYS;

    cJSON *cals = cJSON_GetObjectItemCaseSensitive(root, "calendars");
    if (cJSON_IsArray(cals)) {
        int i = 0;
        cJSON *jc;
        cJSON_ArrayForEach(jc, cals) {
            if (i >= APP_SETTINGS_MAX_CALENDARS) {
                break;
            }
            app_calendar_cfg_t *c = &out->calendars[i];
            cJSON *v;
            v = cJSON_GetObjectItemCaseSensitive(jc, "source");
            c->source = (cJSON_IsNumber(v) && (int)v->valuedouble == APP_CAL_SOURCE_ICS)
                            ? APP_CAL_SOURCE_ICS : APP_CAL_SOURCE_GOOGLE;
            v = cJSON_GetObjectItemCaseSensitive(jc, "id");
            if (cJSON_IsString(v) && v->valuestring) {
                strncpy(c->id, v->valuestring, sizeof(c->id) - 1);
            }
            v = cJSON_GetObjectItemCaseSensitive(jc, "label");
            if (cJSON_IsString(v) && v->valuestring) {
                strncpy(c->label, v->valuestring, sizeof(c->label) - 1);
            }
            v = cJSON_GetObjectItemCaseSensitive(jc, "color");
            c->color = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0x808080;
            v = cJSON_GetObjectItemCaseSensitive(jc, "enabled");
            c->enabled = cJSON_IsBool(v) ? cJSON_IsTrue(v) : true;
            if (c->id[0] != '\0') {
                i++;
            }
        }
        out->calendar_count = i;
    }

    out->valid = (out->wifi_ssid[0] != '\0' && out->sa_client_email[0] != '\0' &&
                  out->sa_private_key_pem[0] != '\0' && out->calendar_count > 0);
}

esp_err_t provisioning_load(app_settings_t *out)
{
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = 0;
    err = nvs_get_str(h, NVS_KEY_JSON, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc(len);
    if (buf == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(h, NVS_KEY_JSON, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "stored config JSON failed to parse - treating as unset");
        return ESP_ERR_NOT_FOUND;
    }
    json_to_settings(root, out);
    cJSON_Delete(root);

    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t provisioning_save(const app_settings_t *cfg)
{
    cJSON *root = settings_to_json(cfg);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        free(str);
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_JSON, str);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    free(str);

    ESP_LOGI(TAG, "config %s", err == ESP_OK ? "saved" : "save FAILED");

    /* Keep the TF card backup in sync with whatever's now authoritative
     * in NVS, not just a one-time snapshot - a config change here (the
     * on-device settings dialog, the LAN config web server, the setup
     * portal) that never made it to the card would mean the card's
     * fallback copy goes stale and could hand back an outdated config
     * the next time NVS needs recovering from it. Best-effort: no card
     * mounted (or no gcal/ dir/file there yet) just means this quietly
     * does nothing rather than failing the NVS save that already
     * succeeded above - provisioning_save_sd() itself no-ops safely via
     * fopen() failing when there's no /sdcard. */
    if (err == ESP_OK && provisioning_save_sd(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "TF card config backup not updated (no card mounted, or write failed)");
    }

    return err;
}

esp_err_t provisioning_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t provisioning_sd_ensure_dir(void)
{
    struct stat st;
    if (stat(PROVISIONING_SD_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s exists but isn't a directory", PROVISIONING_SD_DIR);
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(PROVISIONING_SD_DIR, 0775) != 0) {
        ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d", PROVISIONING_SD_DIR, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "created %s on the TF card", PROVISIONING_SD_DIR);
    return ESP_OK;
}

bool provisioning_sd_config_exists(void)
{
    struct stat st;
    return stat(SD_CONFIG_FILE, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t provisioning_load_sd(app_settings_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(SD_CONFIG_FILE, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s failed to parse - treating as unset", SD_CONFIG_FILE);
        return ESP_ERR_NOT_FOUND;
    }
    json_to_settings(root, out);
    cJSON_Delete(root);

    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t provisioning_save_sd(const app_settings_t *cfg)
{
    cJSON *root = settings_to_json(cfg);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(SD_CONFIG_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s, \"w\") failed: errno=%d", SD_CONFIG_FILE, errno);
        free(str);
        return ESP_FAIL;
    }
    size_t len = strlen(str);
    size_t written = fwrite(str, 1, len, f);
    fclose(f);
    free(str);

    if (written != len) {
        ESP_LOGE(TAG, "short write to %s (%d/%d bytes)", SD_CONFIG_FILE, (int)written, (int)len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "config backed up to %s", SD_CONFIG_FILE);
    return ESP_OK;
}
