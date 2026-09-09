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
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s (is the BH1750/GY-30 wired to the "
                      "shared I2C bus, ADDR pin low, at address 0x23?)",
                 esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    uint8_t power_on = BH1750_CMD_POWER_ON;
    err = i2c_master_transmit(s_dev, &power_on, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power-on command failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t mode = BH1750_CMD_CONT_H_RES_MODE;
    err = i2c_master_transmit(s_dev, &mode, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode command failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Max conversion time for H-Res mode per the datasheet is 180ms - wait
     * it out here so a read coming almost immediately after this returns
     * doesn't just get the sensor's power-on-reset garbage value. */
    vTaskDelay(pdMS_TO_TICKS(180));

    ESP_LOGI(TAG, "BH1750 ready (continuous H-res mode)");
    return ESP_OK;
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
