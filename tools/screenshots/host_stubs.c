/* Stand-ins for the hardware and network modules the UI code calls. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "demo.h"

time_t host_time(time_t *t)
{
    time_t n = demo_now();
    if (t != NULL) {
        *t = n;
    }
    return n;
}

bool bsp_lvgl_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
esp_err_t bsp_display_set_brightness_permille(uint16_t p) { (void)p; return ESP_OK; }
esp_err_t bsp_display_fade_brightness_permille(uint16_t p, uint32_t ms) { (void)p; (void)ms; return ESP_OK; }
esp_err_t bsp_display_backlight(bool on) { (void)on; return ESP_OK; }
void *bsp_get_i2c_bus(void) { return 0; }

esp_err_t light_sensor_init(void *bus) { (void)bus; return ESP_FAIL; }
esp_err_t light_sensor_read_lux(float *lux) { (void)lux; return ESP_FAIL; }
bool presence_sensor_is_detected(void) { return true; }
void ui_settings_dialog_show(void) {}
