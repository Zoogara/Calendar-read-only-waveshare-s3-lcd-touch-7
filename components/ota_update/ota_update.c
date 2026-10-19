#include "ota_update.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

bool ota_update_url_is_insecure(const char *url)
{
    return url != NULL && strncasecmp(url, "https://", 8) != 0;
}

bool ota_update_http_allowed_in_build(void)
{
#ifdef CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
    return true;
#else
    return false;
#endif
}

/* The shared download. `allow_http` is only ever true for the authenticated /ota trigger. */
static esp_err_t apply(const char *url, bool allow_http)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ota_update_url_is_insecure(url) && !(allow_http && ota_update_http_allowed_in_build())) {
        ESP_LOGW(TAG, "refusing a plain-http update URL (%s): firmware updates need https://%s", url,
                 allow_http ? " - this is not a development build" : "");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* With app rollback on, esp_ota_begin() refuses (ESP_ERR_OTA_ROLLBACK_INVALID_STATE)
     * while the running image is still pending verification - i.e. in the first
     * minute after a restart, before ota_update_confirm_boot_after() fires. An
     * update was asked for and this image is up and talking to us, so confirm it
     * now rather than turn the request down. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running != NULL && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running image not yet confirmed - confirming it before the update");
        esp_ota_mark_app_valid_cancel_rollback();
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

esp_err_t ota_update_check_and_apply(const char *url)
{
    return apply(url, false);
}

static void background_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err = apply(url, true);
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
    if (ota_update_url_is_insecure(url) && !ota_update_http_allowed_in_build()) {
        ESP_LOGW(TAG, "refusing a plain-http update URL: this build is https-only");
        return ESP_ERR_NOT_SUPPORTED;
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
