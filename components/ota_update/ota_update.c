#include "ota_update.h"
#include <string.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "ota_update";

esp_err_t ota_update_check_and_apply(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "fetching update from %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        /* Same reasoning as the calendar/token requests in gcal_client.c -
         * a too-small request buffer silently truncates headers rather
         * than failing cleanly. */
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA image written, ready to restart");
    return ESP_OK;
}
