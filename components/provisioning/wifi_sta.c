#include "wifi_sta.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static int s_retry_count;
static bool s_wifi_started;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Keep retrying forever - this is a 24/7 wall display, not a
         * one-shot connect. We still raise WIFI_FAIL_BIT once after
         * MAX_RETRY *consecutive* attempts purely so wifi_sta_connect()'s
         * blocking wait gives up and lets main.c's outer retry loop
         * log/back off instead of blocking forever on e.g. a wrong
         * password - reconnection attempts continue regardless. */
        esp_wifi_connect();
        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi dropped, reconnecting (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(const app_settings_t *cfg, uint32_t timeout_ms)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
    }
    /* Clear any stale bits from a previous failed attempt before we wait
     * again below. */
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (!s_wifi_started) {
        /* One-time bring-up. Deliberately NOT repeated on retries -
         * calling esp_wifi_init()/esp_wifi_start() again on an already
         * running driver would fail; reconnection after this point is
         * handled entirely by event_handler(), forever. */
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, cfg->wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, cfg->wifi_password, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = strlen(cfg->wifi_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
        ESP_LOGI(TAG, "connecting to \"%s\"...", cfg->wifi_ssid);
    } else {
        /* Already running (this is a retry after an earlier timeout) -
         * event_handler is already reconnecting on its own; just wait
         * again. */
        s_retry_count = 0;
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to connect to \"%s\" within %lu ms", cfg->wifi_ssid, (unsigned long)timeout_ms);
    return ESP_FAIL;
}
