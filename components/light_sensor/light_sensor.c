#include "light_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "light_sensor";

#define BH1750_I2C_ADDR            0x23
#define BH1750_CMD_POWER_ON        0x01
#define BH1750_CMD_CONT_H_RES_MODE 0x10

static i2c_master_dev_handle_t s_dev;
static bool s_last_read_ok = true; /* assume ok right after a successful init */

esp_err_t light_sensor_init(i2c_master_bus_handle_t bus)
{
    /* No BH1750 fitted in this build - report it as absent without
     * touching the I2C bus. */
    (void)bus;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t light_sensor_read_lux(float *out_lux)
{
    if (s_dev == NULL || out_lux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[2];
    esp_err_t err = i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        /* Edge-triggered, not every failed read - ambient_brightness_tick()
         * calls this about once a second, and a genuinely dead/unplugged
         * sensor would otherwise spam a log line every second forever.
         * Logging the ok->failing and failing->ok transitions is enough to
         * see "the wire came loose at boot ~14:32" or "it recovered" in the
         * log without drowning everything else out. */
        if (s_last_read_ok) {
            ESP_LOGW(TAG, "read failed: %s (sensor stopped responding? check "
                          "wiring) - auto-dimming frozen at its last value "
                          "until this recovers", esp_err_to_name(err));
            s_last_read_ok = false;
        }
        return err;
    }
    if (!s_last_read_ok) {
        ESP_LOGI(TAG, "read recovered");
        s_last_read_ok = true;
    }

    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    /* Default measurement-time register (MTreg=69) - raw/1.2 is the
     * datasheet's lux conversion for H-Res/L-Res modes at that default
     * (only Mode2 uses raw/2.4, and this driver doesn't use Mode2). */
    *out_lux = (float)raw / 1.2f;
    return ESP_OK;
}
