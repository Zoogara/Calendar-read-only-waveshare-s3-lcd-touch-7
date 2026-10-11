#include "ota_update.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static void background_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err = ota_update_check_and_apply(url);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update applied, restarting");
        vTaskDelay(pdMS_TO_TICKS(1000)); /* let the log forwarder drain */
        esp_restart();
    }
    ESP_LOGW(TAG, "OTA update failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

esp_err_t ota_update_start_background(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(url);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Internal-RAM stack on purpose - see ota_update_check_and_apply(). */
    if (xTaskCreate(background_task, "ota_bg", 8192, copy, 5, NULL) != pdPASS) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void confirm_boot_cb(void *arg)
{
    (void)arg;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "boot confirmed - this image is now the permanent one");
    }
}

void ota_update_confirm_boot_after(uint32_t seconds)
{
    static esp_timer_handle_t s_timer;
    const esp_timer_create_args_t args = {.callback = confirm_boot_cb, .name = "ota_confirm"};
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_once(s_timer, (uint64_t)seconds * 1000000ULL);
    }
}
