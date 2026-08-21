#include "ch422g.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ch422g";

/* CH422G "sub-addresses" - each behaves like a distinct I2C device address
 * rather than a register offset on one address. */
#define CH422G_I2C_ADDR_MODE   0x24   /* system/mode register (EXIO0-7 dir, etc.) */
#define CH422G_I2C_ADDR_OUT0_7 0x38   /* output data register for EXIO0-7 */
#define CH422G_I2C_ADDR_IN0_7  0x26   /* input data register for EXIO0-7 */

#define CH422G_MODE_OUTPUT_0_7 0x01   /* EXIO0-7 = push-pull outputs */

struct ch422g_dev_t {
    i2c_master_dev_handle_t dev_mode;
    i2c_master_dev_handle_t dev_out;
    uint8_t out_shadow;      /* last value written to the output register */
    SemaphoreHandle_t lock;
};

static esp_err_t i2c_write_single(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, pdMS_TO_TICKS(100));
}

esp_err_t ch422g_init(i2c_master_bus_handle_t bus, ch422g_handle_t *out_handle)
{
    if (bus == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ch422g_dev_t *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t mode_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_I2C_ADDR_MODE,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t out_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_I2C_ADDR_OUT0_7,
        .scl_speed_hz = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &mode_cfg, &h->dev_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add mode device failed: %s", esp_err_to_name(err));
        free(h);
        return err;
    }
    err = i2c_master_bus_add_device(bus, &out_cfg, &h->dev_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add output device failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(h->dev_mode);
        free(h);
        return err;
    }

    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        i2c_master_bus_rm_device(h->dev_mode);
        i2c_master_bus_rm_device(h->dev_out);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    /* Put EXIO0-7 into output mode before we drive anything. */
    err = i2c_write_single(h->dev_mode, CH422G_MODE_OUTPUT_0_7);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode-register write failed: %s (is the CH422G on the "
                      "I2C bus / wired correctly?)", esp_err_to_name(err));
        vSemaphoreDelete(h->lock);
        i2c_master_bus_rm_device(h->dev_mode);
        i2c_master_bus_rm_device(h->dev_out);
        free(h);
        return err;
    }

    /* Start with everything low: backlight off, touch held in reset. */
    h->out_shadow = 0x00;
    err = i2c_write_single(h->dev_out, h->out_shadow);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial output write failed: %s", esp_err_to_name(err));
    }

    *out_handle = h;
    ESP_LOGI(TAG, "CH422G ready");
    return ESP_OK;
}

esp_err_t ch422g_set_level(ch422g_handle_t handle, int exio_pin, bool level)
{
    if (handle == NULL || exio_pin < 0 || exio_pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    uint8_t next = handle->out_shadow;
    if (level) {
        next |= (1u << exio_pin);
    } else {
        next &= ~(1u << exio_pin);
    }
    esp_err_t err = ESP_OK;
    if (next != handle->out_shadow) {
        err = i2c_write_single(handle->dev_out, next);
        if (err == ESP_OK) {
            handle->out_shadow = next;
        }
    }
    xSemaphoreGive(handle->lock);
    return err;
}
