#include "presence_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "presence_sensor";

#define PRESENCE_SENSOR_GPIO 6

void presence_sensor_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PRESENCE_SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool presence_sensor_is_detected(void)
{
    static int s_last_logged = -1; /* force a first log line regardless of initial level */
    int level = gpio_get_level(PRESENCE_SENSOR_GPIO);
    if (level != s_last_logged) {
        ESP_LOGI(TAG, "%s", level ? "DETECTED" : "clear");
        s_last_logged = level;
    }
    return level != 0;
}
